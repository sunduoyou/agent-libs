#include "monitor.h"
#include "configuration.h"
#include "Poco/Exception.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "subprocesses_logger.h"
#include <thread>

static int g_signal_received = 0;

static void g_monitor_signal_callback(int sig)
{
	if(g_signal_received == 0)
	{
		g_signal_received = sig;
	}
}

static void create_pid_file(const string& pidfile)
{
	if(!pidfile.empty())
	{
		std::ofstream ostr(pidfile);
		if(ostr.good())
		{
			ostr << Poco::Process::id() << std::endl;
		}
	}
}

static void delete_pid_file(const string& pidfile)
{
	if(!pidfile.empty())
	{
		try
		{
			File f(pidfile);
			f.remove(true);
		}
		catch(Poco::Exception&)
		{
		}
	}
}

void run_sdjagent(shared_ptr<pipe_manager> jmx_pipes)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	jmx_pipes->attach_child_stdio();
	File sdjagent_jar("/opt/draios/share/sdjagent.jar");
	if(sdjagent_jar.exists())
	{
		execl("/usr/bin/java", "java", "-Djava.library.path=/opt/draios/lib", "-jar", "/opt/draios/share/sdjagent.jar", (char *) NULL);
	}
	else
	{
		execl("/usr/bin/java", "java", "-Djava.library.path=../sdjagent", "-jar",
				"../sdjagent/java/sdjagent-1.0-jar-with-dependencies.jar", (char *) NULL);
	}
	std::cerr << "{ \"level\": \"SEVERE\", \"message\": \"Cannot load sdjagent, errno: " << errno <<"\" }" << std::endl;
	exit(EXIT_FAILURE);
}

void monitored_process::exec()
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	// TODO: may be useful rename process?
	m_exec();
}

int monitor::run()
{
	signal(SIGINT, g_monitor_signal_callback);
	signal(SIGQUIT, g_monitor_signal_callback);
	signal(SIGTERM, g_monitor_signal_callback);
	signal(SIGUSR1, SIG_IGN);

	for(auto& process : m_processes)
	{
		auto child_pid = fork();
		if(child_pid < 0)
		{
			exit(EXIT_FAILURE);
		}
		else if(child_pid == 0)
		{
			process.exec();
		}
		else
		{
			process.set_pid(child_pid);
		}
	}

	create_pid_file(m_pidfile);

	while(g_signal_received == 0)
	{
		int status = 0;
		for(auto& process : m_processes)
		{
			auto waited_pid = waitpid(process.pid(), &status, WNOHANG);
			if(waited_pid < 0)
			{
				delete_pid_file(m_pidfile);
				exit(EXIT_FAILURE);
			}
			else if(waited_pid > 0)
			{
				if(process.is_main() && WIFEXITED(status) && WEXITSTATUS(status) == 0)
				{
					//
					// Process terminated cleanly
					//
					delete_pid_file(m_pidfile);
					exit(EXIT_SUCCESS);
				}

				// crashed, restart it
				this_thread::sleep_for(chrono::seconds(1));

				// Notify main process to send log report
				if(!process.is_main())
				{
					for(auto& process : m_processes)
					{
						if(process.is_main())
						{
							kill(process.pid(), SIGUSR2);
							break;
						}
					}
				}

				auto child_pid = fork();
				if (child_pid < 0)
				{
					exit(EXIT_FAILURE);
				}
				else if (child_pid == 0)
				{
					process.exec();
				}
				else
				{
					process.set_pid(child_pid);
				}
			}
		}
		this_thread::sleep_for(chrono::seconds(1));
	}

	for(auto& process : m_processes)
	{
		//
		// Signal received, forward it to the child and
		// wait for it to terminate
		//
		if(kill(process.pid(), g_signal_received) != 0)
		{
			delete_pid_file(m_pidfile);
			exit(EXIT_FAILURE);
		}
		waitpid(process.pid(), NULL, 0);
	}

	delete_pid_file(m_pidfile);
	return(EXIT_SUCCESS);
}

void run_monitor(const string& pidfile, shared_ptr<pipe_manager> jmx_pipes)
{
	signal(SIGINT, g_monitor_signal_callback);
	signal(SIGQUIT, g_monitor_signal_callback);
	signal(SIGTERM, g_monitor_signal_callback);
	signal(SIGUSR1, SIG_IGN);

	//
	// Start the monitor process
	// 
	pid_t child_pid = fork();
	if(child_pid < 0)
	{
		exit(EXIT_FAILURE);
	}

	if(child_pid == 0)
	{
		//
		// Child. Continue execution
		//
		return;
	}

	// Start also sdjagent
	pid_t sdjagent_child_pid = 0;
	if(jmx_pipes)
	{
		sdjagent_child_pid = fork();
		if(sdjagent_child_pid < 0)
		{
			exit(EXIT_FAILURE);
		}

		if(sdjagent_child_pid == 0)
		{
			run_sdjagent(jmx_pipes);
		}
	}

	//
	// Father. It will be the monitor process
	//
	create_pid_file(pidfile);

	while(g_signal_received == 0)
	{
		int status = 0;
		pid_t waited_pid = waitpid(child_pid, &status, WNOHANG);

		if(waited_pid < 0)
		{
			delete_pid_file(pidfile);
			exit(EXIT_FAILURE);
		}

		if(waited_pid == 0)
		{
			//
			// dragent Child still alive
			//
			if(jmx_pipes)
			{
				// check also sdjagent
				waited_pid = waitpid(sdjagent_child_pid, &status, WNOHANG);

				if(waited_pid < 0)
				{
					delete_pid_file(pidfile);
					exit(EXIT_FAILURE);
				}

				if(waited_pid == 0)
				{
					sleep(1);
					continue;
				}

				sdjagent_child_pid = fork();
				if(sdjagent_child_pid < 0)
				{
					exit(EXIT_FAILURE);
				}

				if(sdjagent_child_pid == 0)
				{
					run_sdjagent(jmx_pipes);
				}
			}
			sleep(1);
			continue;
		}

		child_pid = 0;

		if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		{
			//
			// Process terminated cleanly
			//
			delete_pid_file(pidfile);
			exit(EXIT_SUCCESS);
		}

		//
		// Process terminated abnormally, restart it
		//
		sleep(1);

		child_pid = fork();
		if(child_pid < 0)
		{
			delete_pid_file(pidfile);
			exit(EXIT_FAILURE);
		}

		if(child_pid == 0)
		{
			//
			// Child. Continue execution
			//
			return;
		}
	}

	if(child_pid)
	{
		//
		// Signal received, forward it to the child and
		// wait for it to terminate
		//
		if(kill(child_pid, g_signal_received) != 0)
		{
			delete_pid_file(pidfile);
			exit(EXIT_FAILURE);
		}

		waitpid(child_pid, NULL, 0);
	}

	if(sdjagent_child_pid)
	{
		if(kill(sdjagent_child_pid, g_signal_received) != 0)
		{
			delete_pid_file(pidfile);
			exit(EXIT_FAILURE);
		}

		waitpid(sdjagent_child_pid, NULL, 0);
	}

	delete_pid_file(pidfile);
	exit(EXIT_SUCCESS);
}
