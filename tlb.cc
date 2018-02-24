/** tlb.cc
    author: yq
    date: Februry 2018
*/

#include "tlb.h"

/**************************MSHR******************************/
// check if there is a pending request to lower TLB level or page table
bool tlb_mshr::probe(new_addr_type va) const
{
	mshr_table::const_iterator a = mshr_table.find(va);
	return a != mshr_table.end();
}

// check if there is space for a new tlb access
bool tlb_mshr::full(new_addr_type va) const
{
	mshr_table::const_iterator i = mshr_table.find(va);
	if (i != mshr_table.end()) {
		return i->second.m_list.size() >= m_num_targets;
	} else {
		return mshr_table.size() >= m_num_entries;
	}
}

// add or merge this tlb access
void tlb_mshr::add(new_addr_type va, entry_info info)
{
	mshr_table[va].m_list.push_back(info);
	assert(mshr_table.size() <= n_num_entries);
	assert(mshr_table[va].m_list.size() <= n_num_targets);
}

// accept a new TLB fill response: mark entry ready for processing
void tlb_mshr::mark_ready(new_addr_type va, new_addr_type pa)
{
	table::iterator a = mshr_table.find(va);
	assert(a != mshr_table.end());
	res_info res;
	res.va = va;
	res.pa = pa;
	m_current_response.push_back(res);
	assert(m_current_response.size() <= mshr_table.size());
}

// return next ready access
entry_info tlb_mshr::next_access()
{
	assert(access_ready());
	res_info res = m_current_response.front();
	new_addr_type va = res->va;
	assert(!mshr_table[va].m_list.empty());
	entry_info result = mshr_table[va].m_list.front(); // get missed access
	result.mf->set_pa(res->pa); // set m_pa
	mshr_table[va].m_list.pop_front();
	if (mshr_table[va].m_list.empty()) {
		mshr_table.erase(va);
		m_current_response.pop_front();
	}
	return result;
}

/*********************************TLB**********************************/
void tlb::get_sub_stats(struct tlb_sub_stats &tss)
{
	tss.accesses = m_access;
	tss.misses = m_miss;
	tss.pending_hits = m_pending_hit;
	tss.res_fails = m_res_fail;
}

enum tlb_request_status tlb::probe(mem_fetch *mf, unsigned &idx) const;
{
	// calculate index
	new_addr_type addr = mf->get_addr();
	unsigned set_index = m_config.get_index(addr);
	new_addr_type va = m_config.get_va(addr);
	mf->set_va(va);
	
	unsigned invalid_line = (unsigned) - 1;
	unsigned valid_line = (unsigned) - 1;
	unsigned valid_timestamp = (unsigned) - 1;
	bool all_reserved = true;
	
	// check for result
	for (unsigned way = 0; way < m_config.m_assoc; way++) {
		unsigned index = set_index * config.m_assoc + way;
		tlb_entry_t *entry = &m_entries[index];
		if (va == entry->m_va) { 
			if (entry->m_state == RESERVED) { // hit-reserved
				idx = index;
				return HIT_RESERVED;
			} else if (entry->m_state == VALID) { // hit
				idx = index;
				return HIT;
			}	else { // it seems that this is impossible?
				assert(entry->m_state == INVALID);
			}
		}
		if (entry->m_state != RESERVED) {
			all_reserved = false;
			if (entry->m_state == INVALID) {
				invalid_line = index;
			} else {
				// valid entry: keep track of most appropriate replacement candidate
				if (m_config.m_replacement_policy == LRU) { // LRU
					if (entry->m_last_access_time < valid_timestamp) { 
						valid_timestamp = entry->m_last_access_time;
						valid_line = index;
					}
				} else if (m_config.m_replacement_policy == FIFO) { // FIFO
					if (entry->m_alloc_time < valid_timestamp) {
						valid_timestamp = entry->m_alloc_time;
						valid_line = index;
					}
				}
			}
		}
	}
	if (all_reserved) {
		return RESERVATION_FAIL; // not enough entries in TLB to allocate on miss
	}
	if (invalid_line != (unsigned) - 1) {
		idx = invalid_line;
	} else if (valid_line != (unsigned) - 1) {
		idx = valid_line;
	}	else {
		abort();
	}
	return MISS;
}

enum tlb_request_status tlb::access(mem_fetch *mf, unsigned time, unsigned &idx) // no need to write back
{
	m_access++;
	//shader_tlb_access_log();
	enum tlb_request_status status = probe(mf, idx);
	switch (status) {
		case HIT_RESERVED:
			m_pending_hit++;
			m_entries[idx].m_last_access_time = time;
			break;
		case HIT:
			m_entries[idx].m_last_access_time = time;
			mf->set_pa(m_entries[idx].m_pa);
			break;
		case MISS:
			m_miss++;
		  //shader_tlb_access_log();
			m_entries[idx].allocate(mf->get_va(), time);
			break;
		case RESERVATION_FAIL:
			m_res_fail++;
			break;
		default:
			fprintf(stderr, "tlb::access - Error: Unknown tlb_request_status %d\n", status);
			abort();
	}
	return status;
}

void tlb::fill(mem_fetch *mf, unsigned index, unsigned time) 
{
	m_entries[index].fill(mf, time);
}

/******************************l1_tlb*****************************/
l1_tlb::port_management::port_management()
{
	m_data_port_occupied_cycles = 0;
	m_fill_port_occupied_cycles = 0;
}

void l1_tlb::port_management::use_data_port()
{
	m_data_port_occupied_cycles += 1;
}

void l1_tlb::port_management::use_fill_port()
{
	m_fill_port_occupied_cycles += 1;
}

void l1_tlb::port_management::replenish_port()
{
	if (m_data_port_occupied_cycles > 0) {
		m_data_port_occupied_cycles -= 1;
	}
	assert(m_data_port_occupied_cycles >= 0);

	if (m_fill_port_occupied_cycles > 0) {
		m_fill_port_occupied_cycles -= 1;
	}
	assert(m_fill_port_occupied_cycles >= 0);
}

bool l1_tlb::port_management::data_port_free()
{
	return (m_data_port_occupied_cycles < m_config.port_count);
}

bool l1_tlb::port_management::fill_port_free()
{
	return (m_fill_port_occupied_cycles == 0);
}

void l1_tlb::cycle() // move miss request from miss queue to L2 TLB
{
	if (!m_miss_queue.empty() && m_L2TLB->serve_port_free()) { // check L2 TLB serve port
		mem_fetch *mf = m_miss_queue.front();
		delay_t d;
		d.req = mf;
		d.ready_cycle = gpu_sim_cycle + gpu_tot_sim_cycle + m_config->m_access_latency;
		m_L2TLB->m_delay_queue.push_back(d);
		m_L2TLB->use_serve_port();
	}
	port.replenish_port(); 
}

// process TLB access
enum tlb_request_status l1_tlb::process_access(mem_fetch *mf, unsigned time)
{
	unsigned index;
	enum tlb_request_status status = access(mf, time, index);
	if (status == HIT) {
		convert_addr(mf); //To-do, convert virtual tag to physical tag.
    m_response_queue.push_back(mf);
		port.use_data_port(); // occupy data port
	} else if (status == HIT_RESERVED || status == MISS) {
    new_addr_type va = mf->get_va();
		entry_info info;
		info.mf = mf;
		info.index = index;
		info.status = status;
    bool mshr_hit = m_mshrs.probe(va);
		bool mshr_avail = !m_mshrs.full(va);
		if (mshr_hit && mshr_avail) {
			m_mshrs.add(va, info);
		} else if (!mshr_hit && mshr_avail && (m_miss_queue.size() < m_config.m_miss_size()) {
			m_mshrs.add(va, info);
			m_miss_queue.push_back(mf);
		} else {
			status = RESERVATION_FAIL;
			m_res_fail++;
		}
	}
	return status;
}

void l1_tlb::writeback()
{
	// m_current_response is not empty, process miss access
  if (m_mshrs->access_ready()) {
    entry_info info = m_mshrs.next_access();
    fill(info.mf, info.index, time);
    convert_addr(info.mf); // To-do
    m_response_queue.push_back(info.mf);
  }
}

// process request in mshr and fill TLB
void l1_tlb::fill(unsigned time)
{
	// get response from L2TLB
	if (!m_L2TLB->m_response_queue[get_coreId()].m_list.empty() &&
			m_L2TLB->data_port_free()) { // check L2 TLB port
		mem_fetch *mf = m_response_queue[get_coreId()].m_list.front();
		m_mshrs.mark_ready(mf->get_va(), mf->get_pa());
		m_response_queue[get_coreId()].m_list.pop_front();
		port.use_fill_port(); // occupy fill port
		m_L2TLB->use_data_port();
	}
}

/******************************l2_tlb*****************************/
l2_tlb::port_management::port_management()
{
  m_data_port_occupied_cycles = 0;
	m_serve_port_occupied_cycles = 0;
  m_fill_port_occupied_cycles = 0;
}

void l2_tlb::port_management::use_data_port()
{
  m_data_port_occupied_cycles += 1;
}

void l2_tlb::port_management::use_fill_port()
{
  m_fill_port_occupied_cycles += 1;
}

void l2_tlb::port_management::use_serve_port()
{
	m_serve_port_occupied_cycles += 1;
}

void l2_tlb::port_management::replenish_port()
{
  if (m_data_port_occupied_cycles > 0) {
    m_data_port_occupied_cycles -= 1;
  }
  assert(m_data_port_occupied_cycles >= 0);
	
	if (m_serve_port_occupied_cycles > 0) {
		m_serve_port_occupied_cycles -= 1;
	}
	assert(m_serve_port_occupied_cycles >= 0);

  if (m_fill_port_occupied_cycles > 0) {
    m_fill_port_occupied_cycles -= 1;
  }
  assert(m_fill_port_occupied_cycles >= 0);
}

bool l2_tlb::port_management::data_port_free()
{
  return (m_data_port_occupied_cycles < m_config.port_count);
}

bool l2_tlb::port_management::serve_port_free()
{
  return (m_serve_port_occupied_cycles < m_config.port_count);
}

bool l2_tlb::port_management::fill_port_free()
{
  return (m_fill_port_occupied_cycles == 0);
}

void l2_tlb::process_access(unsigned time)
{
	if (!m_delay_queue.empty() && (time >= m_delay_queue.front().ready_cycle)) {
		mem_fetch *mf = m_delay_queue.req;
		//m_delay_queue.pop();
		unsigned index;
		enum tlb_request_status status = access(mf, time, index);
		if (status == HIT) {
			convert_addr(mf); //To-do
			m_response_queue[mf->get_sid()].m_list.push_back(mf);
			m_delay_queue.pop();
		} else if (status == HIT_RESERVED || status == MISS) {
			new_addr_type va = mf->get_va();
			entry_info info;
			info.mf = mf;
			info.index = index;
			info.status = status;
			bool mshr_hit = m_mshrs.probe(va);
			bool mshr_avail = !m_mshrs.full(va);
			if (mshr_hit && mshr_avail) {
      	m_mshrs.add(va, info);
				m_delay_queue.pop();
    	} else if (!mshr_hit && mshr_avail && (m_miss_queue.size() < m_config.m_miss_size()) {
      	m_mshrs.add(va, info);
      	m_miss_queue.push_back(mf);
				m_delay_queue.pop();
			} else {
				status = RESERVATION_FAIL;
				m_res_fail++;
			}
    }
		//return status;
	}
}

void l2_tlb::cycle()
{
	if (!m_miss_queue.empty()) {
    mem_fetch *mf = m_miss_queue.front();
		m_miss_queue.pop_front();
    //To-do page walk
  }	
	replenish_port();	
}

void l2_tlb::writeback()
{
	// m_current_response is not empty, process miss access
  if (m_mshrs->access_ready()) {
    entry_info info = m_mshrs.next_access();
    if (info.status == MISS) {
      fill(info.mf, info.index, time);
    }
    convert_addr(info.mf); // To-do
    m_response_queue[mf->get_sid()].m_list.push_back(info->mf);
  }
}

void l2_tlb::fill(unsigned time)
{
	mem_fetch *mf =  // To-do, get response from page walk
  m_mshrs.mark_ready(mf->get_va(), mf->get_pa());
	port.use_fill_port();
	// pop request
}
