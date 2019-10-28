#include <tuples.h>
#include "secure_audit.h"

namespace
{
const uint32_t LOCAL_IP_ADDRESS = 16777343; // 127.0.0.1
const int DEFAULT_AUDIT_FREQUENCY_S = 10;
const uint64_t FREQUENCY_THRESHOLD_NS = 100000000; // 100 ms

uint64_t seconds_to_ns(const int seconds)
{
	return ((uint64_t)seconds * ONE_SECOND_IN_NS);
}
} // namespace

type_config<bool> secure_audit::c_secure_audit_enabled(
	false,
	"Secure Audit Enabled",
	"secure_audit_streams",
	"enabled");

type_config<bool> secure_audit::c_secure_audit_executed_commands_enabled(
	false,
	"If true, secure_audit reports executed commands",
	"secure_audit_streams",
	"executed_commands");

//Per-second Per-container limit
type_config<int> secure_audit::c_secure_audit_executed_commands_per_container_limit(
	30,
	"The maximum number of executed commands that secure_audit will report per second, per-container",
	"secure_audit_streams",
	"executed_commands_per_container_limit");

type_config<bool> secure_audit::c_secure_audit_connections_enabled(
	false,
	"If true, secure_audit will report new connections",
	"secure_audit_streams",
	"connections");

type_config<bool> secure_audit::c_secure_audit_connections_local(
	false,
	"If secure audit connection monitoring is enabled and this is true, secure_audit will report loopback connections",
	"secure_audit_streams",
	"connections_local");

type_config<bool> secure_audit::c_secure_audit_connections_cmdline(
	false,
	"If true, secure_audit will enrich connections with command lines of the process starting the connection itself",
	"secure_audit_streams",
	"connections_cmdline");

type_config<int> secure_audit::c_secure_audit_connections_cmdline_maxlen(
	30,
	"If secure_audit command line reporting is enabled, the maximum length of each command line that secure_audit will report",
	"secure_audit_streams",
	"connections_cmdline_maxlen");

type_config<bool> secure_audit::c_secure_audit_connections_only_interactive(
	true,
	"If true, secure_audit will send out connections only generated by interactive shells",
	"secure_audit_streams",
	"connections_only_interactive");

type_config<bool> secure_audit::c_secure_audit_k8s_audit_enabled(
	false,
	"If true, secure_audit will monitor k8s audit events",
	"secure_audit_streams",
	"k8s_audit");

type_config<int>::mutable_ptr secure_audit::c_secure_audit_frequency =
	type_config_builder<int>(
		DEFAULT_AUDIT_FREQUENCY_S,
		"If secure audit is enabled, set the protobuf frequency in seconds (0 means at every flush)",
		"secure_audit_streams",
		"frequency")
		.min(1)
		.max(30 * 60) // 30 min
		.build_mutable();

type_config<int> secure_audit::c_secure_audit_executed_commands_limit(
	1000,
	"limit on numbers of executed commands in every message sent - 0 means no limit",
	"secure_audit_streams",
	"executed_commands_limit");

type_config<int> secure_audit::c_secure_audit_connections_limit(
	1000,
	"limit on numbers of connections in every message sent - 0 means no limit",
	"secure_audit_streams",
	"connections_limit");

type_config<int> secure_audit::c_secure_audit_k8s_limit(
	200,
	"limit on numbers of k8s audit in every message sent - 0 means no limit"
	"secure_audit_streams",
	"k8s_limit");

secure_audit::secure_audit():
	m_secure_audit_batch(new secure::Audit),
	m_get_events_interval(make_unique<run_on_interval>(seconds_to_ns(DEFAULT_AUDIT_FREQUENCY_S), FREQUENCY_THRESHOLD_NS)),
	m_executed_commands_count(0),
	m_connections_count(0),
	m_k8s_audit_count(0),
	m_executed_commands_dropped_count(0),
	m_connections_dropped_count(0),
	m_k8s_audit_dropped_count(0),
	m_connections_not_interactive_dropped_count(0),
	m_k8s_audit_enrich_errors_count(0)
{
	clear();
}

void secure_audit::init(sinsp_ipv4_connection_manager* conn)
{
	m_connection_manager = conn;

	m_get_events_interval->interval(seconds_to_ns(c_secure_audit_frequency->get_value()));
	m_get_events_interval->threshold(FREQUENCY_THRESHOLD_NS);

	if(c_secure_audit_connections_enabled.get_value())
	{
		if(m_connection_manager == nullptr)
		{
			SINSP_WARNING("secure_audit failed registering add connection callback");
			return;
		}

		// Register callback
		m_connection_manager->subscribe_on_new_tcp_connection([this](const _ipv4tuple& tuple,
									     sinsp_connection& conn,
									     sinsp_connection::state_transition transition) {
			emit_connection_async(tuple, conn, transition);
		});
	}
}

secure_audit::~secure_audit()
{
	delete m_secure_audit_batch;
}

void secure_audit::set_data_handler(secure_audit_data_ready_handler* handler)
{
	m_audit_data_handler = handler;
}

void secure_audit::set_internal_metrics(secure_audit_internal_metrics* internal_metrics)
{
	m_audit_internal_metrics = internal_metrics;
}

const secure::Audit* secure_audit::get_events(uint64_t timestamp)
{
	if(!c_secure_audit_enabled.get_value())
	{
		return nullptr;
	}

	if(m_secure_audit_batch->connections_size() == 0 &&
	   m_secure_audit_batch->executed_commands_size() == 0 &&
	   m_secure_audit_batch->k8s_audits_size() == 0)
	{
		g_logger.format(sinsp_logger::SEV_DEBUG, "No secure audit messages generated");
		return nullptr;
	}
	m_secure_audit_batch->set_timestamp(timestamp);
	m_secure_audit_batch->set_hostname(sinsp_gethostname());
	return m_secure_audit_batch;
}

void secure_audit::clear()
{
	m_secure_audit_batch->Clear();
}

void secure_audit::flush(uint64_t ts)
{
	secure_audit_sent = false;
	secure_audit_run = false;

	m_get_events_interval->run([this, ts]() {
		uint64_t flush_start_time = sinsp_utils::get_current_time_ns();
		secure_audit_run = true;

		auto secure_audits = get_events(ts);

		if(secure_audits)
		{
			m_audit_data_handler->secure_audit_data_ready(ts, secure_audits);
			secure_audit_sent = true;
		}

		clear();

		uint64_t flush_time_ms = (sinsp_utils::get_current_time_ns() - flush_start_time) / 1000000;

		if(secure_audit_sent)
		{
			m_audit_internal_metrics->set_secure_audit_internal_metrics(1, flush_time_ms);
			g_logger.format(sinsp_logger::SEV_INFO, "secure_audit: flushing fl.ms=%d ", flush_time_ms);
		}
		m_audit_internal_metrics->set_secure_audit_sent_counters(m_executed_commands_count,
									 m_connections_count,
									 m_k8s_audit_count,
									 m_executed_commands_dropped_count,
									 m_connections_dropped_count,
									 m_k8s_audit_dropped_count,
									 m_connections_not_interactive_dropped_count,
									 m_k8s_audit_enrich_errors_count);
		reset_counters();
	},
				   ts);

	if(!secure_audit_sent)
	{
		m_audit_internal_metrics->set_secure_audit_internal_metrics(0, 0);
	}
	if(!secure_audit_run)
	{
		m_audit_internal_metrics->set_secure_audit_sent_counters(0, 0, 0, 0, 0, 0, 0, 0);
	}
}

void secure_audit::reset_counters()
{
	m_executed_commands_count = 0;
	m_executed_commands_dropped_count = 0;
	m_connections_count = 0;
	m_connections_dropped_count = 0;
	m_k8s_audit_count = 0;
	m_k8s_audit_dropped_count = 0;
	m_connections_not_interactive_dropped_count = 0;
	m_k8s_audit_enrich_errors_count = 0;
}

secure::CommandCategory command_category_to_secure_audit_enum(draiosproto::command_category& tcat)
{
	// Explicitly converting to point out mismatches
	secure::CommandCategory cat;

	switch(tcat)
	{
	case draiosproto::CAT_NONE:
		cat = secure::CommandCategory::COMMAND_CATEGORY_NONE;
		break;
	case draiosproto::CAT_CONTAINER:
		cat = secure::CommandCategory::COMMAND_CATEGORY_CONTAINER;
		break;
	case draiosproto::CAT_HEALTHCHECK:
		cat = secure::CommandCategory::COMMAND_CATEGORY_HEALTHCHECK;
		break;
	case draiosproto::CAT_LIVENESS_PROBE:
		cat = secure::CommandCategory::COMMAND_CATEGORY_LIVENESS_PROBE;
		break;
	case draiosproto::CAT_READINESS_PROBE:
		cat = secure::CommandCategory::COMMAND_CATEGORY_READINESS_PROBE;
		break;
	default:
		g_logger.format(sinsp_logger::SEV_ERROR, "Unknown command category, using CAT_NONE");
		cat = secure::CommandCategory::COMMAND_CATEGORY_NONE;
	}

	return cat;
}

bool executed_command_cmp_secure(const sinsp_executed_command& src, const sinsp_executed_command& dst)
{
	return (src.m_ts < dst.m_ts);
}

void secure_audit::emit_commands_audit(std::unordered_map<std::string, std::vector<sinsp_executed_command>>* executed_commands)
{
	if(!(c_secure_audit_enabled.get_value() &&
	     c_secure_audit_executed_commands_enabled.get_value()))
	{
		return;
	}

	int executed_commands_size_initial = m_secure_audit_batch->executed_commands_size();

	std::unordered_map<std::string, std::vector<sinsp_executed_command>>::iterator it = executed_commands->begin();

	while(it != executed_commands->end())
	{
		emit_commands_audit_item(&(it->second), it->first);
		it++;
	}

	int executed_commands_size_final = m_secure_audit_batch->executed_commands_size();

	g_logger.format(sinsp_logger::SEV_DEBUG, "secure_audit: emit commands audit (%d) - batch size (%d -> %d)",
			executed_commands_size_final - executed_commands_size_initial,
			executed_commands_size_initial,
			executed_commands_size_final);
}

void secure_audit::emit_commands_audit_item(vector<sinsp_executed_command>* commands, const std::string& container_id)
{
	if(commands->size() != 0)
	{
		if(c_secure_audit_executed_commands_limit.get_value() != 0 &&
		   m_executed_commands_count > c_secure_audit_executed_commands_limit.get_value())
		{
			m_executed_commands_count += commands->size();
			return;
		}

		sort(commands->begin(),
		     commands->end(),
		     executed_command_cmp_secure);

		//
		// if there are too many commands, try to aggregate by command line
		//
		uint32_t cmdcnt = 0;

		vector<sinsp_executed_command>::iterator it;

		for(it = commands->begin(); it != commands->end(); ++it)
		{
			if(!(it->m_flags & sinsp_executed_command::FL_EXCLUDED))
			{
				cmdcnt++;
			}
		}

		if(c_secure_audit_executed_commands_per_container_limit.get_value() != 0 &&
		   cmdcnt > c_secure_audit_executed_commands_per_container_limit.get_value())
		{
			map<string, sinsp_executed_command*> cmdlines;

			for(it = commands->begin(); it != commands->end(); ++it)
			{
				if(!(it->m_flags & sinsp_executed_command::FL_EXCLUDED))
				{
					map<string, sinsp_executed_command*>::iterator eit = cmdlines.find(it->m_cmdline);
					if(eit == cmdlines.end())
					{
						cmdlines[it->m_cmdline] = &(*it);
					}
					else
					{
						eit->second->m_count++;
						it->m_flags |= sinsp_executed_command::FL_EXCLUDED;
					}
				}
			}
		}

		//
		// if there are STILL too many commands, try to aggregate by executable
		//
		cmdcnt = 0;

		for(it = commands->begin(); it != commands->end(); ++it)
		{
			if(!(it->m_flags & sinsp_executed_command::FL_EXCLUDED))
			{
				cmdcnt++;
			}
		}

		if(c_secure_audit_executed_commands_per_container_limit.get_value() != 0 &&
		   cmdcnt > c_secure_audit_executed_commands_per_container_limit.get_value())
		{
			map<string, sinsp_executed_command*> exes;

			for(it = commands->begin(); it != commands->end(); ++it)
			{
				if(!(it->m_flags & sinsp_executed_command::FL_EXCLUDED))
				{
					map<string, sinsp_executed_command*>::iterator eit = exes.find(it->m_exe);
					if(eit == exes.end())
					{
						exes[it->m_exe] = &(*it);
						it->m_flags |= sinsp_executed_command::FL_EXEONLY;
					}
					else
					{
						eit->second->m_count += it->m_count;
						it->m_flags |= sinsp_executed_command::FL_EXCLUDED;
					}
				}
			}
		}

		cmdcnt = 0;
		for(it = commands->begin(); it != commands->end(); ++it)
		{
			if(!(it->m_flags & sinsp_executed_command::FL_EXCLUDED))
			{
				cmdcnt++;

				if(c_secure_audit_executed_commands_limit.get_value() != 0 &&
				   (m_executed_commands_count > c_secure_audit_executed_commands_limit.get_value()))
				{
					m_executed_commands_dropped_count++;
					break;
				}

				if(c_secure_audit_executed_commands_per_container_limit.get_value() != 0 &&
				   cmdcnt > c_secure_audit_executed_commands_per_container_limit.get_value())
				{
					m_executed_commands_dropped_count++;
					break;
				}

				auto pb_command_audit = m_secure_audit_batch->add_executed_commands();

				m_executed_commands_count++;

				pb_command_audit->set_timestamp(it->m_ts);
				pb_command_audit->set_count(it->m_count);
				pb_command_audit->set_login_shell_id(it->m_shell_id);
				pb_command_audit->set_login_shell_distance(it->m_login_shell_distance);
				pb_command_audit->set_comm(it->m_comm);
				pb_command_audit->set_pid(it->m_pid);
				pb_command_audit->set_ppid(it->m_ppid);
				pb_command_audit->set_uid(it->m_uid);
				pb_command_audit->set_cwd(it->m_cwd);
				pb_command_audit->set_tty(it->m_tty);

				pb_command_audit->set_category(command_category_to_secure_audit_enum(it->m_category));

				pb_command_audit->set_container_id(container_id);

				if(it->m_flags & sinsp_executed_command::FL_EXEONLY)
				{
					pb_command_audit->set_cmdline(it->m_exe);
				}
				else
				{
					pb_command_audit->set_cmdline(it->m_cmdline);
				}
			}
		}
	}
}

secure::ConnectionStatus connection_status(int errorcode)
{
	return errorcode == 0 ? secure::ConnectionStatus::CONNECTION_STATUS_ESTABLISHED : secure::ConnectionStatus::CONNECTION_STATUS_FAILED;
}

enum ip_proto_l4
{
	IP_PROTO_INVALID = 0,
	IP_PROTO_ICMP = 1,
	IP_PROTO_TCP = 6,
	IP_PROTO_UDP = 17
};

static const std::unordered_map<uint8_t, uint8_t> l4_proto_scap_to_ip_map =
	{
		{SCAP_L4_UNKNOWN, IP_PROTO_INVALID},
		{SCAP_L4_NA, IP_PROTO_INVALID},
		{SCAP_L4_TCP, IP_PROTO_TCP},
		{SCAP_L4_UDP, IP_PROTO_UDP},
		{SCAP_L4_ICMP, IP_PROTO_ICMP},
		{SCAP_L4_RAW, IP_PROTO_INVALID}};

static uint8_t scap_l4_to_ip_l4(const uint8_t scap_l4)
{
	auto it = l4_proto_scap_to_ip_map.find(scap_l4);
	if(it != l4_proto_scap_to_ip_map.end())
	{
		return it->second;
	}
	else
	{
		return IP_PROTO_INVALID;
	}
}

void secure_audit::append_connection(connection_type type,
				     const sinsp_connection::state_transition transition,
				     const _ipv4tuple& tuple, sinsp_connection& conn)
{
	// Avoid to emit local connections (src or dst 127.0.0.1)
	if(!c_secure_audit_connections_local.get_value() &&
	   (tuple.m_fields.m_sip == LOCAL_IP_ADDRESS || tuple.m_fields.m_dip == LOCAL_IP_ADDRESS))
	{
		return;
	}

	sinsp_threadinfo* tinfo = nullptr;
	if(type == connection_type::SRC)
	{
		tinfo = conn.m_sproc.get()->get_main_thread();
	}
	else
	{
		tinfo = conn.m_dproc.get()->get_main_thread();
	}

	if(c_secure_audit_connections_only_interactive.get_value())
	{
		if(tinfo == nullptr)
		{
			return;
		}

		if((tinfo != nullptr) &&
		   (!(tinfo->m_ainfo->m_th_analysis_flags & thread_analyzer_info::flags::AF_IS_INTERACTIVE_COMMAND)))
		{
			m_connections_not_interactive_dropped_count++;
			return;
		}
	}

	secure::ConnectionStatus conn_status = connection_status(transition.error_code);

	// check if connection is not 0.0.0.0:0 -> 0.0.0.0:0
	// this could be caused by agent in subsampling mode
	// if so we simply discard this
	if(tuple.m_fields.m_sip == 0 &&
	   tuple.m_fields.m_dip == 0 &&
	   tuple.m_fields.m_sport == 0 &&
	   tuple.m_fields.m_dport == 0)
	{
		return;
	}

	auto pb_conn = m_secure_audit_batch->add_connections();

	m_connections_count++;

	pb_conn->set_client_ipv4(ntohl(tuple.m_fields.m_sip));
	pb_conn->set_client_port(tuple.m_fields.m_sport);

	pb_conn->set_l4_protocol(scap_l4_to_ip_l4(tuple.m_fields.m_l4proto));

	pb_conn->set_server_ipv4(ntohl(tuple.m_fields.m_dip));
	pb_conn->set_server_port(tuple.m_fields.m_dport);

	pb_conn->set_status(conn_status);
	pb_conn->set_error_code(transition.error_code);
	pb_conn->set_timestamp(transition.timestamp);

	std::string cmdline;

	if(type == connection_type::SRC)
	{
		pb_conn->set_client_pid(conn.m_spid);

		if(conn.m_sproc != nullptr)
		{
			pb_conn->set_comm(conn.m_sproc->get_comm());
			pb_conn->set_container_id(conn.m_sproc.get()->m_container_id);
			if(c_secure_audit_connections_cmdline.get_value())
			{
				sinsp_threadinfo::populate_cmdline(cmdline, conn.m_sproc.get());
			}
		}
	}
	else if(type == connection_type::DST)
	{
		pb_conn->set_server_pid(conn.m_dpid);

		if(conn.m_dproc != nullptr)
		{
			pb_conn->set_comm(conn.m_dproc->get_comm());
			pb_conn->set_container_id(conn.m_dproc.get()->m_container_id);
			if(c_secure_audit_connections_cmdline.get_value())
			{
				sinsp_threadinfo::populate_cmdline(cmdline, conn.m_dproc.get());
			}
		}
	}

	if(c_secure_audit_connections_cmdline.get_value() && !cmdline.empty())
	{
		if(c_secure_audit_connections_cmdline_maxlen.get_value() != 0 && cmdline.length() > c_secure_audit_connections_cmdline_maxlen.get_value())
		{
			cmdline.resize(c_secure_audit_connections_cmdline_maxlen.get_value());
		}
		pb_conn->set_cmdline(cmdline);
	}
}

void secure_audit::emit_connection_async(const _ipv4tuple& tuple, sinsp_connection& conn, sinsp_connection::state_transition transition)
{
	if(!(c_secure_audit_enabled.get_value() && c_secure_audit_connections_enabled.get_value()))
	{
		return;
	}

	if(c_secure_audit_connections_limit.get_value() != 0 &&
	   (m_connections_count > c_secure_audit_connections_limit.get_value()))
	{
		m_connections_dropped_count++;
		return;
	}

	// If client_and_server connection, consider it as a server_only
	// A client_only connection should already been processed first, so metadata (pid, comm, container)
	// related to client side should already been emitted
	if(conn.is_client_and_server())
	{
		append_connection(connection_type::DST, transition, tuple, conn);
	}
	else
	{
		if(conn.is_client_only())
		{
			append_connection(connection_type::SRC, transition, tuple, conn);
		}
		if(conn.is_server_only())
		{
			append_connection(connection_type::DST, transition, tuple, conn);
		}
	}
}

void secure_audit::filter_and_append_k8s_audit(const nlohmann::json& j,
					       std::vector<std::string>& k8s_active_filters,
					       std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& k8s_filters,
					       infrastructure_state *infra_state)
{
	if(!(c_secure_audit_enabled.get_value() && c_secure_audit_k8s_audit_enabled.get_value()))
	{
		return;
	}

	if(filter_k8s_audit(j, k8s_active_filters, k8s_filters))
	{
		if(c_secure_audit_k8s_limit.get_value() != 0 &&
		   (m_k8s_audit_count > c_secure_audit_k8s_limit.get_value()))
		{
			m_k8s_audit_dropped_count++;
			return;
		}

		auto pb_k8s_audit = m_secure_audit_batch->add_k8s_audits();
		pb_k8s_audit->set_blob(j.dump());

		bool ok = infra_state != nullptr;
		try
		{
			nlohmann::json::json_pointer r_jptr("/objectRef/resource");
			if(ok && j.at(r_jptr) == "pods") {
				// if the object of this audit event is a pod,
				// get its container.id and host.hostName
				// from infrastructure state
				nlohmann::json::json_pointer ns_jptr("/objectRef/namespace"), n_jptr("/objectRef/name");
				std::string pod_uid = infra_state->get_k8s_pod_uid(j.at(ns_jptr), j.at(n_jptr));
				if ((ok = !pod_uid.empty()))
				{
					// hostname retrieval
					std::string hostname;
					infrastructure_state::uid_t uid = make_pair("k8s_pod", pod_uid);

					ok &= infra_state->find_tag(uid, "host.hostName", hostname);

					pb_k8s_audit->set_hostname(hostname);

					// containerID retrieval from the pod container name

					// sadly we have to parse the pod container name from the requestURI,
					// there's no other way to retrieve it from the audit event
					nlohmann::json::json_pointer req_jptr("/requestURI");
					std::string requestURI = j.at(req_jptr);

					std::size_t pos = requestURI.find("container=");
					if (pos != std::string::npos) {
						std::string pod_container_name = requestURI.substr(pos-1+sizeof("container="));
						pos = pod_container_name.find("&");
						if (pos != std::string::npos) {
							pod_container_name.resize(pos);
						}

						std::string container_id = infra_state->get_container_id_from_k8s_pod_and_k8s_pod_name(uid, pod_container_name);
						pb_k8s_audit->set_container_id(container_id);
						ok &= !container_id.empty();
					}
				}
			}
		}
		catch(nlohmann::json::out_of_range&)
		{
			// nothing we can do here, container ID and/or hostname will be empty
			ok = false;
		}

		if (!ok) {
			m_k8s_audit_enrich_errors_count++;
		}

		m_k8s_audit_count++;
	}
}

bool secure_audit::filter_k8s_audit(const nlohmann::json& j,
				    std::vector<std::string>& k8s_active_filters,
				    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& k8s_filters)
{

	// Iterate over active filters
	for(auto active_filter : k8s_active_filters)
	{
		// For each filter do not match and break if at least one condition is not matched
		bool all_matched = true;

		// If filter has at least one field consider the filter valid
		if(k8s_filters[active_filter].size() == 0)
		{
			all_matched = false;
		}

		for(auto filter_item : k8s_filters[active_filter])
		{
			try
			{
				nlohmann::json::json_pointer jptr(filter_item.first);

				if(j.at(jptr) != filter_item.second)
				{
					all_matched = false;
					break;
				}
			}
			catch(nlohmann::json::out_of_range&)
			{
				all_matched = false;
				break;
			}
		}

		if(all_matched)
		{
			return true;
		}
	}

	return false;
}
