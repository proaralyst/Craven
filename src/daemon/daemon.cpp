#include <cstdlib>

#include <iostream>
#include <unordered_map>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/expressions/formatters/date_time.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/attributes/current_thread_id.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include <boost/filesystem.hpp>
#include <boost/asio.hpp>

namespace logging = boost::log;
namespace expr = boost::log::expressions;
namespace fs = boost::filesystem;

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif //HAVE_CONFIG_H

#include "configure.hpp"
#include "daemon.hpp"
#include "fuselink.hpp"

raft::Controller::TimerLength Daemon::timer(
		const std::tuple<uint32_t, uint32_t, uint32_t>& lengths) const
{
	std::random_device rd;
	return raft::Controller::TimerLength{rd(), std::get<0>(lengths),
		std::get<1>(lengths), std::get<2>(lengths)};
}

Daemon::Daemon(DaemonConfigure const& config)
	:io_(),
	id_(config.id()),
	ctx_tick_(io_),
	fst_tick_(io_),
	remcon_(io_, config.socket()),
	pool_(std::bind(&TopLevelDispatch<TCPConnectionPool>::operator(), std::ref(dispatch_),
				std::placeholders::_1, std::placeholders::_2)),
	dispatch_(pool_),
	comms_(id_, io_, config.listen(),
			config.node_info(), pool_),
	raft_(io_, dispatch_, timer(config.raft_timer()),
			id_, config.node_list(),
			config.raft_log().string()),
	changetx_(config.persistence_root(),
			std::bind(changetx_send_, std::placeholders::_1,
				"changetx", std::placeholders::_2)),
	fsstate_(raft_.client(), changetx_, id_,
			config.fuse_uid(), config.fuse_gid())
{
	//Setup our logs
	init_log(config.log_path(), config.output_loudness(), config.log_level());

	if(config.version_requested())
	{
		std::cout << "Distributed Filesystem (c) Tom Johnson 2014\n"
			<< "Project v" << VERSION
			<< " Daemon v0.0\n";
		state_ = exit;
	}
	else
	{

		//register changetx's handlers
		changetx_send_ = dispatch_.connect_dispatcher("changetx",
				[this](const Json::Value& value,
					typename dispatch_type::Callback cb)
				{
					const std::string type =
						json_help::checked_from_json<std::string>(value,
								"type",
								"Bad changetx RPC:");
					if("request" == type)
						cb(changetx_.request(value));
					else if("response" == type)
						changetx_.response(cb.endpoint(), value);
					else
						BOOST_LOG_TRIVIAL(warning)
							<< "Unknown type for changetx rpc: "
							<< type;
				});


		//set up fuselink
		fuselink::io(&io_);
		fuselink::state(&fsstate_);
		fuselink::mount_point(config.fuse_mount().string());

		uint32_t tick_timeout = config.tick_timeout();

		//connection manager & timer
		start_ctx_timer(tick_timeout);

		//filesystem state & timer
		start_fst_timer(tick_timeout);

		// Double-fork to avoid zombification on parent exit
		if(config.daemonise())
			double_fork();


		//fuse thread
		std::thread fuse_thread(fuselink::run_fuse);

		//run the event loop
		while(state_ == running)
		{
			try
			{
				io_.run();
			}
			catch(std::exception& ex)
			{
				BOOST_LOG_TRIVIAL(error) << "Caught exception at top level: "
					<< ex.what();
			}
		}

		BOOST_LOG_TRIVIAL(info) << "Waiting for fuse thread to join...";

		if(fuse_thread.joinable())
			fuse_thread.join();
	}
}

int Daemon::exit_code() const
{
	return static_cast<int>(state_);
}


void Daemon::double_fork() const
{
	pid_t pid1, pid2;

	if((pid1 = fork())) // parent process
		std::exit(0);
	else if (!pid1) // child process
	{
		setsid();
		if((pid2 = fork())) // second parent
			std::exit(0);
		else if(!pid2)
		{
			// Change current directory
			chdir("/");

			// Reset our umask:
			umask(0);

			// Close stdin, stdout and stderr:
			close(0);
			close(1);
			close(2);
		}
		else
			BOOST_LOG_TRIVIAL(warning) << "Second fork of daemonise failed. Continuing...";
	}
	else
		BOOST_LOG_TRIVIAL(warning) << "First fork of daemonise failed. Continuing...";
}

void Daemon::init_log(fs::path const& log_path, DaemonConfigure::loudness stderr_level, logging::trivial::severity_level level) const
{
	//Add attributes

	//Add LineID, TimeStamp, ProcessID and ThreadID.
	logging::add_common_attributes();

	//Formatter
	auto formatter = expr::stream
		<< "[" << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%dT%H:%M:%S%q")
		<< "] {" << expr::attr<logging::attributes::current_thread_id::value_type>("ThreadID")
		<< "} (" << logging::trivial::severity
		<< "): " << expr::message;


	if(stderr_level != DaemonConfigure::daemon)
	{
		logging::trivial::severity_level stderr_severity;
		switch(stderr_level)
		{
		case DaemonConfigure::quiet:
			stderr_severity = logging::trivial::fatal;
			break;

		case DaemonConfigure::verbose:
			stderr_severity = logging::trivial::info;
			break;

		case DaemonConfigure::normal:
		default:
			stderr_severity = logging::trivial::warning;
		}

		logging::add_console_log(std::cerr, logging::keywords::format = formatter)->set_filter(logging::trivial::severity >= stderr_severity);
	}

	logging::add_file_log(logging::keywords::file_name = log_path.string(),
			logging::keywords::open_mode = std::ios::app,
			logging::keywords::format = formatter)
		->set_filter(logging::trivial::severity >= level);


	BOOST_LOG_TRIVIAL(info) << "Log start.";
	BOOST_LOG_TRIVIAL(info) << "Logging to " << log_path;
}

void Daemon::start_ctx_timer(uint32_t tick_timeout)
{
	ctx_tick_.expires_from_now(boost::posix_time::milliseconds(tick_timeout));
	ctx_tick_.async_wait(
			[this, tick_timeout](const boost::system::error_code& ec)
			{
				if(!ec)
					changetx_.tick();
				else
					BOOST_LOG_TRIVIAL(error) << "Timer for changetx tick failed: "
						<< ec.message();

				if(ec != boost::asio::error::operation_aborted)
					start_ctx_timer(tick_timeout);
			});
}

void Daemon::start_fst_timer(uint32_t tick_timeout)
{
	fst_tick_.expires_from_now(boost::posix_time::milliseconds(tick_timeout));
	fst_tick_.async_wait(
			[this, tick_timeout](const boost::system::error_code& ec)
			{
				if(!ec)
					fsstate_.tick();
				else
					BOOST_LOG_TRIVIAL(error) << "Timer for fsstate tick failed: "
						<< ec.message();

				if(ec != boost::asio::error::operation_aborted)
					start_fst_timer(tick_timeout);
			});
}
