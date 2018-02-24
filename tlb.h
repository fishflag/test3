/** tlb.h
		author: yq
		date: Februry 2018
*/

#ifndef GPU_TLB_H
#define GPU_TLB_H

#include <stdio.h>
#include <stdlib.h>
#include "mem_fetch.h"
#include "addrdec.h"

enum tlb_entry_state
{
	RESERVED,
	INVALID,
	VALID
};

enum tlb_request_status
{
	HIT = 0,
	HIT_RESERVED,
	MISS,
	RESERVATION_FAIL,
	NUM_TLB_REQUEST_STATUS
};

enum replacement_policy_t
{
	LRU,
	FIFO
};

/**************tlb entry*******************/
struct tlb_entry_t
{
	new_addr_type m_va;
	new_addr_type m_pa;
	unsigned m_alloc_time;
	unsigned m_last_access_time;
	unsigned m_fill_time;
	tlb_entry_state m_state;

	tlb_entry_t()
	{
		m_va = 0;
		m_pa = 0;
		m_alloc_time = 0;
		m_last_access_time = 0;
		m_fill_time = 0;
		m_state = INVALID;
	}
	void allocate(new_addr_type va, unsigned time)
	{
		m_va = va;
		m_pa = 0;
		m_alloc_time = time;
		m_last_access_time = time;
		m_fill_time = 0;
		m_state = RESERVED;
	}
	void fill(mem_fetch *mf, unsigned time)
	{
		if (m_pa != mf->get_pa()) {
			m_pa = mf->get_pa();
			m_fill_time = time;
			m_state = VALID;
		}
	}	
};

/**********************tlb config**************************/
class tlb_config
{
private:
	bool m_valid;
	bool m_disabled;
	unsigned m_page_size;
	unsigned m_nset;
	unsigned m_assoc;
	unsigned m_nset_log2;
	unsigned m_page_sz_log2;
	unsigned m_access_latency;
	unsigned m_num_entries;
	unsigned m_num_targets;
	unsigned m_miss_queue_size;
	unsigned port_count;
	char *m_config_string;
	enum replacement_policy_t m_replacement_policy;
	
	friend class tlb;
	friend class l1_tlb;
	friend class l2_tlb;

public:
	tlb_config()
	{
		m_valid = false;
		m_disabled = false;
		m_config_string = NULL;
	}
	void init(char *config)
	{
		char rp; // for replacement policy
		int len = sscanf(config, "%u:%u:%u:%u:%c:%u:%u:%u:%u", &m_page_size, &m_nset, &m_assoc, &m_access_latency, 
										 &rp, &m_num_entries, &m_num_targets, &m_miss_queue_size, &port_count);
		if (len < 9) {
			exit_parse_error();
		}
		switch (rp) {
			case 'L':
				m_replacement_policy = LRU;
				break;
			case 'F':
				m_replacement_policy = FIFO;
				break;
			default:
				exit_parse_error();
		}
		m_nset_log2 = LOGB2(m_nset);
		m_page_sz_log2 = LOGB2(m_page_size);
		m_valid = true;
	}
	new_addr_type get_va(new_addr_type addr) const  // TO-DO. How to get virtual address?
	{
	} 
	new_addr_type get_index(new_addr_type addr) const // To-do
	{
		
	}
	unsigned get_num_entries() const
	{
		return m_nset * m_assoc;
	}
	void exit_parse_error()
	{
		printf("TLB configuration parsing error (%s)\n", m_config_string);
		abort();
	}
};

class l1_tlb_config : public tlb_config
{
public:
	l1_tlb_config() : tlb_config()
	{
	}	
};

class l2_tlb_config :  public tlb_config
{
public:
	l2_tlb_config() : tlb_config()
	{
	}
};
/********************TLB MSHR***********************/
class tlb_mshr
{
private:
	unsigned m_num_entries;
	unsigned m_num_targets;
	
	struct entry_info
	{
		mem_fetch *mf;
		unsigned index;
		enum tlb_request_status status;
	};
	struct mshr_entry
	{
		std::list<entry_info> m_list;
	};
	typedef tr1_hash_map<new_addr_type, mshr_entry> table;
	table mshr_table;
	
	struct res_info
	{
		new_addr_type va;
		new_addr_type pa;
	};
	//bool m_current_response_ready;
	std::list<res_info> m_current_response;

public:
	tlb_mshr(unsigned num_entries, unsigned num_targets)
	{
		m_num_entries = num_entries;
		m_num_targets = num_targets;
	}
	bool probe(new_addr_type va) const;
	bool full(new_addr_type va) const;
	void add(new_addr_type va, entry_info info);
	void mark_ready(new_addr_type va, new_addr_type pa);
	entry_info next_access();
	//void display(FILE *fp) const;
	bool access_ready() const
	{
		return !m_current_response_empty();
	}	
};
/*************************TLB stats ************************/
struct tlb_sub_stats
{
	unsigned accesses;
	unsigned misses;
	unsigned pending_hits;
	unsigned res_fails;
	tlb_sub_stats ()
	{
		clear();
	}
	void clear() 
	{
		accesses = 0;
		misses = 0;
		pending_hits = 0;
		res_fails = 0;
	}
	tlb_sub_stats &operator+=(const tlb_sub_stats &tss)
	{
		accesses += css.accesses;
		misses += css.misses;
		pending_hits += css.pending_hits;
		res_fails += css.res_fails;
		return *this;
	}
	tlb_sub_stats operator+(const tlb_sub_stats &tss)
	{
		tlb_sub_stats ret;
		ret.accesses = accesses + tss.accesses;
		ret.misses = misses + tss.misses;
		ret.pending_hits = pending_hits + tss.pending_hits;
		ret.res_fails = res_fails + tss.res_fails;
	}
};
/*****************************TLB***************************/
class tlb
{
private:
	char *m_name;
	tlb_config &m_config;
	tlb_entry_t *m_entries;
	tlb_mshr m_mshrs;
	std::list<mem_fetch*> m_miss_queue;

	unsigned m_access;
	unsigned m_miss;
	unsigned m_pending_hit;
	unsigned m_res_fail;

	//unsigned m_core_id;	

public:
	tlb(char *name, tlb_config &config) :
		m_config(config), m_mshrs(config.m_num_entries, config.m_num_targets)
	{
		m_name = name;
		//m_core_id = core_id;
		m_entries = new tlb_entry_t[config.get_num_entries()];
		m_access = 0;
		m_miss = 0;
		m_pending_hit = 0;
		m_res_fail = 0;	
	}	
	~tlb() 
	{
		delete[] m_entries;
	}
	
	enum tlb_request_status probe(mem_fetch *mf, unsigned &idx) const;
	enum tlb_request_status access(mem_fetch *mf, unsigned time, unsigned &idx);
	void fill(mem_fetch *mf, unsigned index, unsigned time);
	void flush();
	void print(FILE *stream, unsigned &total_accesses, unsigned &total_misses) const;
	void get_stats(unsigned &total_accesses, unsigned &total_misses, unsigned &total_hit_res,
								 unsigned &total_res_fail) const;
	void get_sub_stats(struct tlb_sub_stats &tss);
};

/*********************L1 TLB**********************/
class l1_tlb : public tlb
{
private:
	unsigned m_core_id;
	l2_tlb *m_L2TLB;
	//l1_cache *m_L1D;

	// port management
	class port_management
	{
		private:
			//tlb_config &m_config;
			int m_data_port_occupied_cycles;
			int m_fill_port_occupied_cycles;
		public:
			port_management();
			void use_data_port();
			void use_fill_port();
			void replenish_port();
			bool data_port_free();
			bool fill_port_free();
	};
	port_management port;

public:
	fifo_pipeline<mem_fetch*> m_response_queue;

	l1_tlb(char *name, tlb_config &config, int core_id, l2_tlb *L2TLB) :
				 tlb(name, config) 
	{
		m_core_id = core_id;
		m_L2TLB = L2TLB;
	}
	void cycle();
	void writeback();
	void fill(unsigned time);
	void process_access(mem_fetch *mf, unsigned time);
	bool data_port_free() const
	{
		return port.data_port_free();
	}
	bool fill_port_free() const
	{	
		return port.fill_port_free();
	}
	unsigned get_coreId() const
	{
		return m_core_id;
	}
};

/********************************L2 TLB******************************/
class l2_tlb : public tlb
{
private:
	//l1_tlb **m_L1TLB;
	mmu *m_page_manager;
	
	struct delay_t
	{
		unsigned long long ready_cycle;
		mem_fetch *req;
	};
	std::queue<delay_t> m_delay_queue;

	// port management
  class port_management
  {
    private:
      //tlb_config &m_config;
      int m_data_port_occupied_cycles;
			int m_serve_port_occupied_cycles;
      int m_fill_port_occupied_cycles;
    public:
      port_management();
      void use_data_port();
      void use_fill_port();
			void use_serve_port();
      void replenish_port();
      bool data_port_free();
      bool fill_port_free();
			bool serve_port_free();
  };
  port_management port;

public:
	struct response_entry
  {
    std::list<mem_fetch*> m_list;
  };
  typedef tr1_hash_map<unsigned, response_entry> queue;
  queue m_response_queue;

	l2_tlb(char *name, tlb_config &config, mmu *page_manager) :
				tlb(name, config) 
	{
		m_page_manager = page_manager;
	}		
	//void add(l1_tlb *L1TLB, unsigned sid);
	void cycle();
	void writeback();
	enum tlb_request_status process_access(unsigned time);
	void fill(unsigned time);
	void use_serve_port()
	{
		port.use_serve_port();
	}
	void use_data_port() 
	{
		port.use_data_port();
	}
	bool data_port_free() const
	{
		return port.data_port_free();
	}
	bool fill_port_free() const
	{
		return port.fill_port_free();
	}
	bool serve_port_free() const
	{
		return port.serve_port_free();
	}
};






#endif
