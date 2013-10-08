#pragma once

class sinsp_scores;
class sinsp_procfs_parser;

typedef union _process_tuple
{
	struct 
	{
		uint64_t m_spid;
		uint64_t m_dpid;
		uint32_t m_sip;
		uint32_t m_dip;
		uint16_t m_sport;
		uint16_t m_dport;
		uint8_t m_l4proto;
	}m_fields;
	uint8_t m_all[29];
}process_tuple;

struct process_tuple_hash
{
	size_t operator()(process_tuple t) const
	{
		size_t seed = 0;

		std::hash<uint64_t> hasher64;
		std::hash<uint32_t> hasher32;
		std::hash<uint8_t> hasher8;

		seed ^= hasher64(*(uint64_t*)t.m_all) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher64(*(uint64_t*)t.m_all + 8) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher64(*(uint64_t*)t.m_all + 16) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher32(*(uint32_t*)(t.m_all + 24)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher8(*(uint8_t*)(t.m_all + 28)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);

		return seed;
	}
};

struct process_tuple_cmp
{
	bool operator () (process_tuple t1, process_tuple t2) const
	{
		return (memcmp(t1.m_all, t2.m_all, sizeof(t1.m_all)) == 0);
	}
};

//
// The main analyzer class
//
class sinsp_analyzer
{
public:
	sinsp_analyzer(sinsp* inspector);
	~sinsp_analyzer();

	void set_sample_callback(analyzer_callback_interface* cb);
	//
	// Processing entry point
	//
	void process_event(sinsp_evt* evt);
	void add_syscall_time(sinsp_counters* metrics, 
		sinsp_evt::category* cat, 
		uint64_t delta, 
		uint32_t bytes, 
		bool inc_count);

	uint64_t get_last_sample_time_ns()
	{
		return m_next_flush_time_ns;
	}

VISIBILITY_PRIVATE
	char* serialize_to_bytebuf(OUT uint32_t *len, bool compressed);
	void serialize(uint64_t ts);
	uint64_t compute_process_transaction_delay(sinsp_transaction_counters* trcounters);
	void emit_aggregate_connections();
	void emit_full_connections();
	void flush(sinsp_evt* evt, uint64_t ts, bool is_eof);

	uint64_t m_next_flush_time_ns;
	uint64_t m_prev_flush_time_ns;

	uint64_t m_prev_sample_evtnum;

	//
	// Pointer to context that we use frequently
	//
	sinsp* m_inspector;
	const scap_machine_info* m_machine_info;

	//
	// The score calculation class
	//
	sinsp_scores* m_score_calculator;

	//
	// This is the protobuf class that we use to pack things
	//
	draiosproto::metrics* m_metrics;
	char* m_serialization_buffer;
	uint32_t m_serialization_buffer_size;

	//
	// The callback we invoke when a sample is ready
	//
	analyzer_callback_interface* m_sample_callback;

	//
	// State required for CPU load calculation
	//
	uint64_t m_old_global_total_jiffies;
	sinsp_procfs_parser* m_procfs_parser;
	vector<uint32_t> m_cpu_loads;

	//
	// Syscall error table
	//
	sinsp_error_counters m_host_syscall_errors;

	//
	// The aggregation metrics for outside-subnet connections
	//
//	vector<unordered_map<ipv4tuple, sinsp_connection, ip4t_hash, ip4t_cmp>::iterator> m_connections_to_emit;
	sinsp_ipv4_connection_manager m_aggregated_ipv4_table;
	unordered_map<process_tuple, sinsp_connection, process_tuple_hash, process_tuple_cmp> m_reduced_ipv4_connections;

#ifdef ANALYZER_EMITS_PROGRAMS
	//
	// The temporary table that we build while scanning the process list.
	// Each entry contains a "program", i.e. a group of processes with the same 
	// full executable path.
	//
	unordered_map<string, sinsp_threadinfo*> m_program_table;
#endif
};
