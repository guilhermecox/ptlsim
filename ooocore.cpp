//
// PTLsim: Cycle Accurate x86-64 Simulator
// Out-of-Order Core Simulator
// Core Structures
//
// Copyright 2003-2008 Matt T. Yourst <yourst@yourst.com>
// Copyright 2006-2008 Hui Zeng <hzeng@cs.binghamton.edu>
// Copyright (c) 2007-2012 Advanced Micro Devices, Inc.
// Contributed by Stephan Diestelhorst <stephan.diestelhorst@amd.com>
//

#include <globals.h>
#include <elf.h>
#include <ptlsim.h>
#include <branchpred.h>
#include <datastore.h>
#include <logic.h>
#include <dcache.h>

#define INSIDE_OOOCORE
#define DECLARE_STRUCTURES
#include <ooocore.h>
#include <stats.h>

#ifndef ENABLE_CHECKS
#undef assert
#define assert(x) (x)
#endif

#ifndef ENABLE_LOGGING
#undef logable
#define logable(level) (0)
#endif

using namespace OutOfOrderModel;

namespace OutOfOrderModel {
  byte uop_executable_on_cluster[OP_MAX_OPCODE];
  W32 forward_at_cycle_lut[MAX_CLUSTERS][MAX_FORWARDING_LATENCY+1];
};

void StateList::init(const char* name, ListOfStateLists& lol, W32 flags) {
  this->name = strdup(name);
  this->flags = flags;
  listid = lol.add(this);
  reset();
}

void StateList::reset() {
  selfqueuelink::reset();
  count = 0;
  dispatch_source_counter = 0;
  issue_source_counter = 0;
}

int ListOfStateLists::add(StateList* list) {
  assert(count < lengthof(data));
  data[count] = list;
  return count++;
}

void ListOfStateLists::reset() {
  foreach (i, count) {
    data[i]->reset();
  }
}

//
// Initialize lookup tables used by the simulation
//
static void init_luts() {
  // Initialize opcode maps
  foreach (i, OP_MAX_OPCODE) {
    W32 allowedfu = fuinfo[i].fu;
    W32 allowedcl = 0;
    foreach (cl, MAX_CLUSTERS) {
      if (clusters[cl].fu_mask & allowedfu) setbit(allowedcl, cl);
    }
    uop_executable_on_cluster[i] = allowedcl;
  }
  
  // Initialize forward-at-cycle LUTs
  foreach (srcc, MAX_CLUSTERS) {
    foreach (destc, MAX_CLUSTERS) {
      foreach (lat, MAX_FORWARDING_LATENCY+1) {
        if (lat == intercluster_latency_map[srcc][destc]) {
          setbit(forward_at_cycle_lut[srcc][lat], destc);
        }
      }
    }
  }
}

void ThreadContext::reset() {
  setzero(specrrt);
  setzero(commitrrt);

  setzero(fetchrip);
  current_basic_block = null;
  current_basic_block_transop_index = -1;
  stall_frontend = false;
  stall_on_eom    = false;
  waiting_for_icache_fill = false;
  waiting_for_icache_fill_physaddr = 0;
  fetch_uuid = 0;
  current_icache_block = 0;
  loads_in_flight = 0;
  stores_in_flight = 0;
  prev_interrupts_pending = false;
  handle_interrupt_at_next_eom = false;
  stop_at_next_eom = false;

  last_commit_at_cycle = 0;
  smc_invalidate_pending = 0;
  setzero(smc_invalidate_rvp);
      
  chk_recovery_rip = 0;
  unaligned_ldst_buf.reset();
  consecutive_commits_inside_spinlock = 0;

  total_uops_committed = 0;
  total_insns_committed = 0;
  dispatch_deadlock_countdown = 0;    
  issueq_count = 0;
  queued_mem_lock_release_count = 0;
  branchpred.init();
}

void ThreadContext::init() {
  rob_states.reset();
  //
  // ROB states
  //
  rob_free_list("free", rob_states, 0);
  rob_frontend_list("frontend", rob_states, ROB_STATE_PRE_READY_TO_DISPATCH);
  rob_ready_to_dispatch_list("ready-to-dispatch", rob_states, 0);
  InitClusteredROBList(rob_dispatched_list, "dispatched", ROB_STATE_IN_ISSUE_QUEUE);
  InitClusteredROBList(rob_ready_to_issue_list, "ready-to-issue", ROB_STATE_IN_ISSUE_QUEUE);
  InitClusteredROBList(rob_ready_to_store_list, "ready-to-store", ROB_STATE_IN_ISSUE_QUEUE);
  InitClusteredROBList(rob_ready_to_load_list, "ready-to-load", ROB_STATE_IN_ISSUE_QUEUE);
  InitClusteredROBList(rob_issued_list, "issued", 0);
  InitClusteredROBList(rob_completed_list, "completed", ROB_STATE_READY);
  InitClusteredROBList(rob_ready_to_writeback_list, "ready-to-write", ROB_STATE_READY);
  rob_cache_miss_list("cache-miss", rob_states, 0);
  rob_tlb_miss_list("tlb-miss", rob_states, 0);
  rob_memory_fence_list("memory-fence", rob_states, 0);
  rob_ready_to_commit_queue("ready-to-commit", rob_states, ROB_STATE_READY);

  reset();
}

void OutOfOrderCore::reset() {
  round_robin_tid = 0;
  round_robin_reg_file_offset = 0;
  caches.reset();
  caches.callback = &cache_callbacks;
  setzero(robs_on_fu);
  foreach_issueq(reset(coreid));
  
  reserved_iq_entries = (int)sqrt(ISSUE_QUEUE_SIZE / MAX_THREADS_PER_CORE);
  assert(reserved_iq_entries && reserved_iq_entries < ISSUE_QUEUE_SIZE);

  foreach_issueq(set_reserved_entries(reserved_iq_entries * MAX_THREADS_PER_CORE));
  foreach_issueq(reset_shared_entries());

  unaligned_predictor.reset();

  foreach (i, threadcount) threads[i]->reset();
}

void OutOfOrderCore::init_generic() {
  reset();
}

template <typename T> 
static void OutOfOrderModel::print_list_of_state_lists(ostream& os, const ListOfStateLists& lol, const char* title) {
  os << title, ":", endl;
  foreach (i, lol.count) {
    StateList& list = *lol[i];
    os << list.name, " (", list.count, " entries):", endl;
    int n = 0;
    T* obj;
    foreach_list_mutable(list, obj, entry, nextentry) {
      if ((n % 16) == 0) os << " ";
      os << " ", intstring(obj->index(), -3);
      if (((n % 16) == 15) || (n == list.count-1)) os << endl;
      n++;
    }
    assert(n == list.count);
    os << endl;
    // list.validate();
  }
}

void StateList::checkvalid() {
#if 0
  int realcount = 0;
  selfqueuelink* obj;
  foreach_list_mutable(*this, obj, entry, nextentry) {
    realcount++;
  }
  assert(count == realcount);
#endif
}

void PhysicalRegisterFile::init(const char* name, int coreid, int rfid, int size) {
  assert(rfid < PHYS_REG_FILE_COUNT);
  assert(size <= MAX_PHYS_REG_FILE_SIZE);
  this->size = size;
  this->coreid = coreid;
  this->rfid = rfid;
  this->name = name;
  this->allocations = 0;
  this->frees = 0;

  foreach (i, MAX_PHYSREG_STATE) {
    stringbuf sb;
    sb << name, "-", physreg_state_names[i];
    states[i].init(sb, getcore().physreg_states);
  }

  foreach (i, size) {
    (*this)[i].init(coreid, rfid, i);
  }
}

PhysicalRegister* PhysicalRegisterFile::alloc(W8 threadid, int r) {
  PhysicalRegister* physreg = (PhysicalRegister*)((r == 0) ? &(*this)[r] : states[PHYSREG_FREE].peek());
  if unlikely (!physreg) return null;
  physreg->changestate(PHYSREG_WAITING);
  physreg->flags = FLAG_WAIT;
  physreg->threadid = threadid;
  allocations++;

  assert(states[PHYSREG_FREE].count >= 0);
  return physreg;
}

ostream& PhysicalRegisterFile::print(ostream& os) const {
  os << "PhysicalRegisterFile<", name, ", rfid ", rfid, ", size ", size, ">:", endl;
  foreach (i, size) {
    os << (*this)[i], endl;
  }
  return os;
}

void PhysicalRegisterFile::reset(W8 threadid) {
  foreach (i, size) {
    if ((*this)[i].threadid == threadid) {
      (*this)[i].reset(threadid);
    }
  }
}

void PhysicalRegisterFile::reset() {
  foreach (i, MAX_PHYSREG_STATE) {
    states[i].reset();
  }

  foreach (i, size) {
    (*this)[i].reset(0, false);
  }

}

StateList& PhysicalRegister::get_state_list(int s) const {
  return getcore().physregfiles[rfid].states[s];
}

namespace OutOfOrderModel {
  ostream& operator <<(ostream& os, const PhysicalRegister& physreg) {
    stringbuf sb;
    print_value_and_flags(sb, physreg.data, physreg.flags);
    os << "TH ", physreg.threadid, " rfid ", physreg.rfid;
    os << "  r", intstring(physreg.index(), -3), " state ", padstring(physreg.get_state_list().name, -12), " ", sb;
    if (physreg.rob) os << " rob ", physreg.rob->index(), " (uuid ", physreg.rob->uop.uuid, ")";
    os << " refcount ", physreg.refcount;
    
    return os;
  }

  template <class T>
  bool GenericEventLog<T>::init(size_t bufsize) {
    reset();
    size_t bytes = bufsize * sizeof(T);
    start = (T*)ptl_mm_alloc_private_pages(bytes);
    if unlikely (!start) return false;
    end = start + bufsize;
    tail = start;

    foreach (i, bufsize) start[i].type = EVENT_INVALID;
    return true;
  }

  template <class T>
  void GenericEventLog<T>::reset() {
    if (!start) return;

    size_t bytes = (end - start) * sizeof(T);
    ptl_mm_free_private_pages(start, bytes);
    start = null;
    end = null;
    tail = null;
  }

  template <>
  ostream& GenericEventLog<OutOfOrderCoreEvent>::print(ostream& os, bool only_to_tail) {
    if (tail >= end) tail = start;
    if (tail < start) tail = end;

    OutOfOrderCoreEvent* p = (only_to_tail) ? start : tail;

    W64 cycle = limits<W64>::max;
    size_t bufsize = end - start;

    if (!config.flush_event_log_every_cycle) os << "#-------- Start of event log --------", endl;

    foreach (i, (only_to_tail ? (tail - start) : bufsize)) {
      if unlikely (p >= end) p = start;
      if unlikely (p < start) p = end-1;
      if unlikely (p->type == EVENT_INVALID) {
        p++;
        continue;
      }

      if unlikely (p->cycle != cycle) {
        cycle = p->cycle;
        os << "[core ", coreid, "] Cycle ", cycle, ":", endl;
      }

      p->print(os);
      p++;
    }

    if (!config.flush_event_log_every_cycle) os << "#-------- End of event log --------", endl;

    return os;
  }

  template <class T>
  void GenericEventLog<T>::flush(bool only_to_tail) {
    //if likely (!logable(6)) return;
    if unlikely (!logfile) return;
    if unlikely (!logfile->ok()) return;
    print(*logfile, only_to_tail);
    tail = start;
  }

  template
  void GenericEventLog<FlexibleOOOCoreBinaryEvent>::flush(bool);

  ostream& OutOfOrderCoreEvent::print(ostream& os) const {
    bool ld = isload(uop.opcode);
    bool st = isstore(uop.opcode);
    bool br = isbranch(uop.opcode);
    W32 exception = LO32(commit.state.reg.rddata);
    W32 error_code = HI32(commit.state.reg.rddata);

    stringbuf uopname;
    nameof(uopname, uop);

    os << intstring(uuid, 20), " t", threadid, " ";
    switch (type) {
      //
      // Fetch Events
      //
    case EVENT_FETCH_STALLED:
      os <<  "fetch  frontend stalled"; break;
    case EVENT_FETCH_ICACHE_WAIT:
      os <<  "fetch  rip ", rip, ": wait for icache fill"; break;
    case EVENT_FETCH_FETCHQ_FULL:
      os <<  "fetch  rip ", rip, ": fetchq full"; break;
    case EVENT_FETCH_IQ_QUOTA_FULL:
      os <<  "fetch  rip ", rip, ": issue queue quota full = ", fetch.issueq_count, " "; break;
    case EVENT_FETCH_BOGUS_RIP:
      os <<  "fetch  rip ", rip, ": bogus RIP or decode failed"; break;
    case EVENT_FETCH_ICACHE_MISS:
      os <<  "fetch  rip ", rip, ": wait for icache fill of phys ", (void*)(Waddr)((rip.mfnlo << 12) + lowbits(rip.rip, 12)), " on missbuf ", fetch.missbuf; break;
    case EVENT_FETCH_SPLIT:
      os <<  "fetch  rip ", rip, ": split unaligned load or store ", uop; break;
    case EVENT_FETCH_ASSIST:
      os <<  "fetch  rip ", rip, ": branch into assist microcode: ", uop; break;
    case EVENT_FETCH_TRANSLATE:
      os <<  "xlate  rip ", rip, ": ", fetch.bb_uop_count, " uops"; break;
    case EVENT_FETCH_OK: {
      os <<  "fetch  rip ", rip, ": ", uop,
        " (uopid ", uop.bbindex;
      if (uop.som) os << "; SOM";
      if (uop.eom) os << "; EOM ", uop.bytes, " bytes";
      os << ")";
      if (uop.eom && fetch.predrip) os << " -> pred ", (void*)fetch.predrip;
      if (isload(uop.opcode) | isstore(uop.opcode)) {
        os << "; unaligned pred slot ", OutOfOrderCore::hash_unaligned_predictor_slot(rip), " -> ", uop.unaligned;
      }
      break;
    }
      //
      // Rename Events
      //
    case EVENT_RENAME_FETCHQ_EMPTY:
      os << "rename fetchq empty"; break;
    case EVENT_RENAME_ROB_FULL:
      os <<  "rename ROB full"; break;
    case EVENT_RENAME_PHYSREGS_FULL:
      os <<  "rename physical register file full"; break;
    case EVENT_RENAME_LDQ_FULL:
      os <<  "rename load queue full"; break;
    case EVENT_RENAME_STQ_FULL:
      os <<  "rename store queue full"; break;
    case EVENT_RENAME_MEMQ_FULL:
      os <<  "rename memory queue full"; break;
    case EVENT_RENAME_OK: {
      os <<  "rename rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " r", intstring(physreg, -3), "@", phys_reg_file_names[rfid];
      if (ld|st) os << " lsq", lsq;
      os << " = ";
      foreach (i, MAX_OPERANDS) os << rename.opinfo[i], ((i < MAX_OPERANDS-1) ? " " : "");
      os << "; renamed";
      os << " ", arch_reg_names[uop.rd], " (old r", rename.oldphys, ")";
      if unlikely (!uop.nouserflags) {
        if likely (uop.setflags & SETFLAG_ZF) os << " zf (old r", rename.oldzf, ")";
        if likely (uop.setflags & SETFLAG_CF) os << " cf (old r", rename.oldcf, ")";
        if likely (uop.setflags & SETFLAG_OF) os << " of (old r", rename.oldof, ")";
      }
      break;
    }
    case EVENT_FRONTEND:
      os <<  "front  rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " frontend stage ", (FRONTEND_STAGES - frontend.cycles_left), " of ", FRONTEND_STAGES;
      break;
    case EVENT_CLUSTER_NO_CLUSTER:
    case EVENT_CLUSTER_OK: {
      os << ((type == EVENT_CLUSTER_OK) ? "clustr" : "noclus"), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " allowed FUs = ",
        bitstring(fuinfo[uop.opcode].fu, FU_COUNT, true), " -> clusters ",
        bitstring(select_cluster.allowed_clusters, MAX_CLUSTERS, true), " avail";
      foreach (i, MAX_CLUSTERS) os << " ", select_cluster.iq_avail[i];
      os << "-> ";
      if (type == EVENT_CLUSTER_OK) os << "cluster ", clusters[cluster].name; else os << "-> none"; break;
      break;
    }
    case EVENT_DISPATCH_NO_CLUSTER:
    case EVENT_DISPATCH_OK: {
      os << ((type == EVENT_DISPATCH_OK) ? "disptc" : "nodisp"),  " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " operands ";
      foreach (i, MAX_OPERANDS) os << dispatch.opinfo[i], ((i < MAX_OPERANDS-1) ? " " : "");
      if (type == EVENT_DISPATCH_OK) os << " -> cluster ", clusters[cluster].name; else os << " -> none";
      break;
    }
    case EVENT_ISSUE_NO_FU: {
      os << "issue  rob ", intstring(rob, -3), "(",padstring(uopname,-5),")";
      os << "no FUs available in cluster ", clusters[cluster].name, ": ",
        "fu_avail = ", bitstring(issue.fu_avail, FU_COUNT, true), ", ",
        "op_fu = ", bitstring(fuinfo[uop.opcode].fu, FU_COUNT, true), ", "
        "fu_cl_mask = ", bitstring(clusters[cluster].fu_mask, FU_COUNT, true);
      break;
    }
    case EVENT_ISSUE_OK: {
      stringbuf sb;
      sb << "issue  rob ", intstring(rob, -3), "(",padstring(uopname,-5),")";
      sb << " on ", padstring(fu_names[fu], -4), " in ", padstring(cluster_names[cluster], -4), ": r", intstring(physreg, -3), "@", phys_reg_file_names[rfid];
      sb << " "; print_value_and_flags(sb, issue.state.reg.rddata, issue.state.reg.rdflags); sb << " =";
      sb << " "; print_value_and_flags(sb, issue.operand_data[RA], issue.operand_flags[RA]); sb << ", ";
      sb << " "; print_value_and_flags(sb, issue.operand_data[RB], issue.operand_flags[RB]); sb << ", ";
      sb << " "; print_value_and_flags(sb, issue.operand_data[RC], issue.operand_flags[RC]);
      sb << " (", issue.cycles_left, " cycles left)";
      if (issue.mispredicted) sb << "; mispredicted (real ", (void*)(Waddr)issue.state.reg.rddata, " vs expected ", (void*)(Waddr)issue.predrip, ")";
      os << sb;
      break;
    }
    case EVENT_REPLAY: {
      os << "replay rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " r", intstring(physreg, -3), "@", phys_reg_file_names[rfid],
        " on cluster ", clusters[cluster].name, ": waiting on";
      foreach (i, MAX_OPERANDS) {
        if (!bit(replay.ready, i)) os << " ", replay.opinfo[i];
      }
      break;
    }
    case EVENT_STORE_WAIT: {
      os << "store", (loadstore.load_store_second_phase ? "2" : " "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      os << "wait on ";
      if (!loadstore.rcready) os << " rc";
      if (loadstore.inherit_sfr_used) {
        os << ((loadstore.rcready) ? "" : " and "), loadstore.inherit_sfr,
          " (uuid ", loadstore.inherit_sfr_uuid, ", stq ", loadstore.inherit_sfr_lsq,
          ", rob ", loadstore.inherit_sfr_rob, ", r", loadstore.inherit_sfr_physreg, ")";
      }
      break;
    }
    case EVENT_STORE_PARALLEL_FORWARDING_MATCH: {
      os << "store", (loadstore.load_store_second_phase ? "2" : " "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      os << "ignored parallel forwarding match with ldq ", loadstore.inherit_sfr_lsq,
        " (uuid ", loadstore.inherit_sfr_uuid, " rob", loadstore.inherit_sfr_rob,
        " r", loadstore.inherit_sfr_physreg, ")";
      break;
    }
    case EVENT_STORE_ALIASED_LOAD: {
      os << "store", (loadstore.load_store_second_phase ? "2" : " "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      os << "aliased with ldbuf ", loadstore.inherit_sfr_lsq, " (uuid ", loadstore.inherit_sfr_uuid,
        " rob", loadstore.inherit_sfr_rob, " r", loadstore.inherit_sfr_physreg, ");",
        " (add colliding load rip ", (void*)(Waddr)loadstore.inherit_sfr_rip, "; replay from rip ", rip, ")";
      break;
    }
    case EVENT_STORE_ISSUED: {
      os << "store", (loadstore.load_store_second_phase ? "2" : " "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      if (loadstore.inherit_sfr_used) {
        os << "inherit from ", loadstore.inherit_sfr, " (uuid ", loadstore.inherit_sfr_uuid,
          ", rob", loadstore.inherit_sfr_rob, ", lsq ", loadstore.inherit_sfr_lsq,
          ", r", loadstore.inherit_sfr_physreg, ");";
      }
      os << " <= ", hexstring(loadstore.data_to_store, 8*(1<<uop.size)), " = ", loadstore.sfr;
      break;
    }
    case EVENT_STORE_LOCK_RELEASED: {
      os << "lk-rel", " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "lock released (original ld.acq uuid ", loadstore.locking_uuid, " rob ", loadstore.locking_rob, " on vcpu ", loadstore.locking_vcpuid, ")";
      break;
    }
    case EVENT_STORE_LOCK_ANNULLED: {
      os << "lk-anl", " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "lock annulled (original ld.acq uuid ", loadstore.locking_uuid, " rob ", loadstore.locking_rob, " on vcpu ", loadstore.locking_vcpuid, ")";
      break;
    }
    case EVENT_STORE_LOCK_REPLAY: {
      os << "store", (loadstore.load_store_second_phase ? "2" : " "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "replay because vcpuid ", loadstore.locking_vcpuid, " uop uuid ", loadstore.locking_uuid, " has lock";
      break;
    }

    case EVENT_LOAD_WAIT: {
      os << (loadstore.load_store_second_phase ? "load2 " : "load  "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      os << "wait on sfr ", loadstore.inherit_sfr,
        " (uuid ", loadstore.inherit_sfr_uuid, ", stq ", loadstore.inherit_sfr_lsq,
        ", rob ", loadstore.inherit_sfr_rob, ", r", loadstore.inherit_sfr_physreg, ")";
      if (loadstore.predicted_alias) os << "; stalled by predicted aliasing";
      break;
    }
    case EVENT_LOAD_HIT:
    case EVENT_LOAD_MISS: {
      if (type == EVENT_LOAD_HIT)
        os << (loadstore.load_store_second_phase ? "load2 " : "load  ");
      else os << (loadstore.load_store_second_phase ? "ldmis2" : "ldmiss");

      os << " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      if (loadstore.inherit_sfr_used) {
        os << "inherit from ", loadstore.inherit_sfr, " (uuid ", loadstore.inherit_sfr_uuid,
          ", rob", loadstore.inherit_sfr_rob, ", lsq ", loadstore.inherit_sfr_lsq,
          ", r", loadstore.inherit_sfr_physreg, "); ";
      }
      if (type == EVENT_LOAD_HIT)
        os << "hit L1: value 0x", hexstring(loadstore.sfr.data, 64);
      else os << "missed L1 (lfrqslot ", lfrqslot, ") [value would be 0x", hexstring(loadstore.sfr.data, 64), "]";
      break;
    }
    case EVENT_LOAD_BANK_CONFLICT: {
      os << "ldbank", " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "L1 bank conflict over bank ", lowbits(loadstore.sfr.physaddr, log2(CacheSubsystem::L1_DCACHE_BANKS));
      break;
    }
    case EVENT_LOAD_TLB_MISS: {
      os << (loadstore.load_store_second_phase ? "ldtlb2" : "ldtlb ");
      os << " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      if (loadstore.inherit_sfr_used) {
        os << "inherit from ", loadstore.inherit_sfr, " (uuid ", loadstore.inherit_sfr_uuid,
          ", rob", loadstore.inherit_sfr_rob, ", lsq ", loadstore.inherit_sfr_lsq,
          ", r", loadstore.inherit_sfr_physreg, "); ";
      }
      else os << "DTLB miss", " [value would be 0x", hexstring(loadstore.sfr.data, 64), "]";
      break;
    }
    case EVENT_LOAD_LOCK_REPLAY: {
      os << (loadstore.load_store_second_phase ? "load2 " : "load  "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "replay because vcpuid ", loadstore.locking_vcpuid, " uop uuid ", loadstore.locking_uuid, " has lock";
      break;
    }
    case EVENT_LOAD_LOCK_OVERFLOW: {
      os << (loadstore.load_store_second_phase ? "load2 " : "load  "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "replay because locking required but no free interlock buffers", endl;
      break;
    }
    case EVENT_LOAD_LOCK_ACQUIRED: {
      os << "lk-acq", " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ",
        "lock acquired";
      break;
    }
    case EVENT_LOAD_LFRQ_FULL:
      os << "load   rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), ": LFRQ or miss buffer full; replaying"; break;
    case EVENT_LOAD_HIGH_ANNULLED: {
      os << (loadstore.load_store_second_phase ? "load2 " : "load  "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, " (phys ", (void*)(Waddr)(loadstore.sfr.physaddr << 3), "): ";
      os << "load was annulled (high unaligned load)";
      break;
    }
    case EVENT_LOAD_WAKEUP:
      os << "ldwake rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), " wakeup load via lfrq slot ", lfrqslot; break;
    case EVENT_TLBWALK_HIT: {
      os << "wlkhit rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), " page table walk (level ",
        loadstore.tlb_walk_level, "): hit for PTE at phys ", (void*)loadstore.virtaddr; break;
      break;
    }
    case EVENT_TLBWALK_MISS: {
      os << "wlkmis rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), " page table walk (level ",
        loadstore.tlb_walk_level, "): miss for PTE at phys ", (void*)loadstore.virtaddr, ": lfrq ", lfrqslot; break;
      break;
    }
    case EVENT_TLBWALK_WAKEUP: {
      os << "wlkwak rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), " page table walk (level ",
        loadstore.tlb_walk_level, "): wakeup from cache miss for phys ", (void*)loadstore.virtaddr, ": lfrq ", lfrqslot; break;
      break;
    }
    case EVENT_TLBWALK_NO_LFRQ_MB: {
      os << "wlknml rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), " page table walk (level ",
        loadstore.tlb_walk_level, "): no LFRQ or MB for PTE at phys ", (void*)loadstore.virtaddr, ": lfrq ", lfrqslot; break;
      break;
    }
    case EVENT_TLBWALK_COMPLETE: {
      os << "wlkhit rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " ldq ", lsq, " r", intstring(physreg, -3), " page table walk (level ",
        loadstore.tlb_walk_level, "): complete!"; break;
      break;
    }
    case EVENT_LOAD_EXCEPTION: {
      os << (loadstore.load_store_second_phase ? "load2 " : "load  "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, ": exception ", exception_name(exception), ", pfec ", PageFaultErrorCode(error_code);
      break;
    }
    case EVENT_STORE_EXCEPTION: {
      os << "store", (loadstore.load_store_second_phase ? "2" : " "), " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " stq ", lsq,
        " r", intstring(physreg, -3), " on ", padstring(fu_names[fu], -4), " @ ",
        (void*)(Waddr)loadstore.virtaddr, ": exception ", exception_name(exception), ", pfec ", PageFaultErrorCode(error_code);
      break;
    }
    case EVENT_ALIGNMENT_FIXUP:
      os << "algnfx", " rip ", rip, ": set unaligned bit for uop ", uop.bbindex, " (unaligned predictor slot ", OutOfOrderCore::hash_unaligned_predictor_slot(rip), ") and refetch"; break;
    case EVENT_FENCE_ISSUED:
      os << "mfence rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " lsq ", lsq, " r", intstring(physreg, -3), ": memory fence (", uop, ")"; break;
    case EVENT_ANNUL_NO_FUTURE_UOPS:
      os << "misspc rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", ": SOM rob ", annul.somidx, ", EOM rob ", annul.eomidx, ": no future uops to annul"; break;
    case EVENT_ANNUL_MISSPECULATION: {
      os << "misspc rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", ": SOM rob ", annul.somidx,
        ", EOM rob ", annul.eomidx, ": annul from rob ", annul.startidx, " to rob ", annul.endidx;
      break;
    }
    case EVENT_ANNUL_EACH_ROB: {
      os << "annul  rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", ": annul rip ", rip;
      os << (uop.som ? " SOM" : "    "); os << (uop.eom ? " EOM" : "    ");
      os << ": free";
      os << " r", physreg;
      if (ld|st) os << " lsq", lsq;
      if (lfrqslot >= 0) os << " lfrq", lfrqslot;
      if (annul.annulras) os << " ras";
      break;
    }
    case EVENT_ANNUL_PSEUDOCOMMIT: {
      os << "pseucm rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", ": r", physreg, " rebuild rrt:";
      os << " arch ", arch_reg_names[uop.rd];
      if likely (!uop.nouserflags) {
        if (uop.setflags & SETFLAG_ZF) os << " zf";
        if (uop.setflags & SETFLAG_CF) os << " cf";
        if (uop.setflags & SETFLAG_OF) os << " of";
      }
      os << " = r", physreg;
      break;
    }
    case EVENT_ANNUL_FETCHQ_RAS:
      os << "anlras rip ", rip, ": annul RAS update still in fetchq"; break;
    case EVENT_ANNUL_FLUSH:
      os << "flush  rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " rip ", rip; break;
    case EVENT_REDISPATCH_DEPENDENTS:
      os << "redisp rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " find all dependents"; break;
    case EVENT_REDISPATCH_DEPENDENTS_DONE:
      os << "redisp rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " redispatched ", (redispatch.count - 1), " dependent uops"; break;
    case EVENT_REDISPATCH_EACH_ROB: {
      os << "redisp rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " from state ", redispatch.current_state_list->name, ": dep on ";
      if (!redispatch.dependent_operands) {
        os << " [self]";
      } else {
        foreach (i, MAX_OPERANDS) {
          if (bit(redispatch.dependent_operands, i)) os << " ", redispatch.opinfo[i];
        }
      }

      os << "; redispatch ";
      os << " [rob ", rob, "]";
      os << " [physreg ", physreg, "]";
      if (ld|st) os << " [lsq ", lsq, "]";
      if (redispatch.iqslot) os << " [iqslot]";
      if (lfrqslot >= 0) os << " [lfrqslot ", lfrqslot, "]";
      if (redispatch.opinfo[RS].physreg != PHYS_REG_NULL) os << " [inheritsfr ", redispatch.opinfo[RS], "]";

      break;
    }
    case EVENT_COMPLETE:
      os << "complt rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " on ", padstring(fu_names[fu], -4), ": r", intstring(physreg, -3); break;
    case EVENT_FORWARD: {
      os << "forwd", forwarding.forward_cycle, " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")",
        " (", clusters[cluster].name, ") r", intstring(physreg, -3),
        " => ", "uuid ", forwarding.target_uuid, " rob ", forwarding.target_rob,
        " (", clusters[forwarding.target_cluster].name, ") r", forwarding.target_physreg,
        " operand ", forwarding.operand;
      if (forwarding.target_st) os << " => st", forwarding.target_lsq;
      os << " [still waiting?";
      foreach (i, MAX_OPERANDS) { if (!bit(forwarding.target_operands_ready, i)) os << " r", (char)('a' + i); }
      if (forwarding.target_all_operands_ready) os << " READY";
      os << "]";
      break;
    }
    case EVENT_BROADCAST: {
      os << "brcst", forwarding.forward_cycle, " rob ", intstring(rob, -3), "(",padstring(uopname,-5),")",
        " from cluster ", clusters[cluster].name, " to cluster ", clusters[forwarding.target_cluster].name,
        " on forwarding cycle ", forwarding.forward_cycle;
      break;
    }
    case EVENT_WRITEBACK: {
      os << "write  rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " (cluster ", clusters[cluster].name, ") r", intstring(physreg, -3), "@", phys_reg_file_names[rfid], " = 0x", hexstring(writeback.data, 64), " ", flagstring(writeback.flags);
      if (writeback.transient) os << " (transient)";
      os << " (", writeback.consumer_count, " consumers";
      if (writeback.all_consumers_sourced_from_bypass) os << ", all from bypass";
      if (writeback.no_branches_between_renamings) os << ", no intervening branches";
      if (writeback.dest_renamed_before_writeback) os << ", dest renamed before writeback";
      os << ")";
      break;
    }
    case EVENT_COMMIT_FENCE_COMPLETED:
      os << "mfcmit rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " fence committed: wake up waiting memory uops"; break;
    case EVENT_COMMIT_EXCEPTION_DETECTED:
      os << "detect rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " exception ", exception_name(exception), " (", exception, "), error code ", hexstring(error_code, 16), ", origvirt ", (void*)(Waddr)commit.origvirt; break;
    case EVENT_COMMIT_EXCEPTION_ACKNOWLEDGED:
      os << "except rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " exception ", exception_name(exception), " [EOM #", commit.total_user_insns_committed, "]"; break;
    case EVENT_COMMIT_SKIPBLOCK:
      os << "skipbk rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " skip block: advance rip by ", uop.bytes, " to ", (void*)(Waddr)(rip.rip + uop.bytes), " [EOM #", commit.total_user_insns_committed, "]"; break;
    case EVENT_COMMIT_SMC_DETECTED:
      os << "smcdet rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " self-modifying code at rip ", rip, " detected (mfn was dirty); invalidate and retry [EOM #", commit.total_user_insns_committed, "]"; break;
    case EVENT_COMMIT_MEM_LOCKED:
      os << "waitlk rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " wait for lock on physaddr ", (void*)(commit.state.st.physaddr << 3), " to be released"; break;
    case EVENT_COMMIT_OK: {
      os << "commit rob ", intstring(rob, -3), "(",padstring(uopname,-5),")";
      if likely (archdest_can_commit[uop.rd])
                  os << " [rrt ", arch_reg_names[uop.rd], " = r", physreg, " 0x", hexstring(commit.state.reg.rddata, 64), "]";

      if ((!uop.nouserflags) && uop.setflags) {
        os << " [flags ", ((uop.setflags & SETFLAG_ZF) ? "z" : ""),
          ((uop.setflags & SETFLAG_CF) ? "c" : ""), ((uop.setflags & SETFLAG_OF) ? "o" : ""),
          " -> ", flagstring(commit.state.reg.rdflags), "]";
      }

      if (uop.eom) os << " [rip = ", (void*)(Waddr)commit.target_rip, commit.krn ? " krn" : "", "]";

      if unlikely (st && (commit.state.st.bytemask != 0))
                    os << " [mem ", (void*)(Waddr)(commit.state.st.physaddr << 3), " = ", bytemaskstring((const byte*)&commit.state.st.data, commit.state.st.bytemask, 8), " mask ", bitstring(commit.state.st.bytemask, 8, true), "]";

      if unlikely (commit.pteupdate.a | commit.pteupdate.d | commit.pteupdate.ptwrite) {
        os << " [pte:";
        if (commit.pteupdate.a) os << " a";
        if (commit.pteupdate.d) os << " d";
        if (commit.pteupdate.ptwrite) os << " w";
        os << "]";
      }

      if unlikely (ld|st) {
        os << " [lsq ", lsq, "]";
        os << " [upslot ", OutOfOrderCore::hash_unaligned_predictor_slot(rip), " = ", commit.ld_st_truly_unaligned, "]";
      }

      if likely (commit.oldphysreg > 0) {
        if unlikely (commit.oldphysreg_refcount) {
          os << " [pending free old r", commit.oldphysreg, " ref by";
          os << " refcount ", commit.oldphysreg_refcount;
          os << "]";
        } else {
          os << " [free old r", commit.oldphysreg, "]";
        }
      }

      os << " [commit r", physreg, "]";

      foreach (i, MAX_OPERANDS) {
        if unlikely (commit.operand_physregs[i] != PHYS_REG_NULL) os << " [unref r", commit.operand_physregs[i], "]";
      }

      if unlikely (br) {
        os << " [brupdate", (commit.taken ? " tk" : " nt"), (commit.predtaken ? " pt" : " np"), ((commit.taken == commit.predtaken) ? " ok" : " MP"), "]";
      }

      if (uop.eom) os << " [EOM #", commit.total_user_insns_committed, "]";
      break;
    }
    case EVENT_COMMIT_ASSIST: {
      assist_func_t assist_func = assistid_to_func[rip.rip];
      os << "assist rob ", intstring(rob, -3), "(",padstring(uopname,-5),")", " calling assist ", (void*)assist_func, " (#",
        assist_index(assist_func), ": ", assist_name(assist_func), ")";
      break;
    }
    case EVENT_RECLAIM_PHYSREG:
      os << "free   r", physreg, " no longer referenced; moving to free state"; break;
    case EVENT_RELEASE_MEM_LOCK: {
      os << "unlkcm", " phys ", (void*)(loadstore.sfr.physaddr << 3), ": lock release committed";
      break;
    }
    default:
      os << "?????? unknown event type ", type;
      break;
    }

    os << endl;
    return os;
  }

  #include <trace_event.h>
  ostream& OutOfOrderCoreBinaryEvent::print_binary(ostream& os, W8 coreid) const {
    bool ld = isload(uop.opcode);
    bool st = isstore(uop.opcode);
    bool br = isbranch(uop.opcode);
    W32 exception = LO32(commit.state.reg.rddata);
    W32 error_code = HI32(commit.state.reg.rddata);

    switch (type) {
    // TODO: Maybe we need the EVENT_LOAD_HIT/MISS event, too?
    case EVENT_COMMIT_OK: {
      // Values
      //   uop.rd                  - Destination arch-register
      //   commit.state.reg.rddata - Data in destination arch-register
      //   - Do we need flags?
      //   Core + cycle generated from outer code!
      //   rip                      - Own RIP
      //   uop.eom                  - Destination register is RIP
      //   commit.target_rip        - Target RIP (for branches interesting!)
      //   commit.state.st.physaddr - Physical address for stores
      //   commit.state.st.data     - Data for stores
      //   commit.state.st.bytemask - Bytemask for stores
      //   commit.state.reg.addr    - Physical address for loads
      //   commit.origvirt          - virtual address (incl. byte offset!)
      //   uop.size                 - Size for loads and stores (2^n bytes)
      //   uop.is_asf               - ASF flag for these instructions
      //   uop.opcode               - differenciate between ASF start / end
      //   threadid                 - thread ID
      //   cycle                    - Simulation cycle

      // Very simple version now: Just threadid, cycle, rip for EOMs
      if (!uop.eom) break;

      TraceEvent rep;
      rep.coreid   = coreid;
      rep.threadid = threadid;
      rep.cycle    = cycle;
      rep.rip      = rip.rip;
      os.write(&rep, sizeof(rep));
      break;
    }
    default:
      break;
    }

    return os;
  }
  ostream& operator<<(ostream& os, const OutOfOrderCoreBinaryEvent& e ) {
    return e.print_binary(os, e.coreid);
  }

  template <>
  ostream& GenericEventLog<FlexibleOOOCoreBinaryEvent>::print(ostream& os, bool only_to_tail) {
    if (tail >= end) tail = start;
    if (tail < start) tail = end;
    FlexibleOOOCoreBinaryEvent* p;

    W64 cycle = limits<W64>::max;
    size_t bufsize = end - start;

    // Print a special event for the coreid.
    MetadataCoreidEvent e = {EVENT_META_COREID, (W16)coreid};
    p = (FlexibleOOOCoreBinaryEvent*) &e;
    p->print_binary(os, coreid);

    p = (only_to_tail) ? start : tail;
    foreach (i, (only_to_tail ? (tail - start) : bufsize)) {
      if unlikely (p >= end) p = start;
      if unlikely (p < start) p = end-1;
      if unlikely (p->type == EVENT_INVALID) {
        p++;
        continue;
      }
      if unlikely (p->cycle != cycle) {
        cycle = p->cycle;
      }

      p->print_binary(os, coreid);
      p++;
    }

    return os;
  }

  class SubfieldSizes {
  private:
    struct TypeSizeMap {
      W16 type_start;
      W16 type_end;
      int size;
    };

    // This maps event IDs to their used structs in the union, if any.
    // Inclusive ranges are used such that multiple types can easily use the same
    // union struct.Keep this in order and dense, it is binary-chopped.
    static const TypeSizeMap sizes[];
    static const int sizes_elem;
    static W16 last_type;
    static int last_size;
    static bool printed_info;

    static int find_chop(int type) {
      // Binary chop
      int i;
      int found = -1;
      int left, right;
      bool out_left, out_right;
      left = 0; right = sizes_elem;

      while (left <= right) {
        i = (left + right) >> 1;
        out_left  = type < sizes[i].type_start;
        out_right = sizes[i].type_end < type;

        if (!out_left & !out_right) {
          found = i;
          break;
        }

        if (out_left) right = i - 1;
        else          left  = i + 1;
      }
      return found;
    }
    static int find_lin(int type) {
      int i;
      for (i = 0; i < sizes_elem; i++)
        if ((sizes[i].type_start <= type) && (type <= sizes[i].type_end))
          return i;
      return -1;
    }
    static void print_sizes() {
      logfile << endl;
      logfile << "Fixed size ", /*offsetof(FlexibleOOOCoreBinaryEvent, start_flexible) +*/ sizeof(W16), endl;
      for (int i = 0; i < sizes_elem; i++)
        logfile << "  ", sizes[i].type_start,"-",sizes[i].type_end,": ", sizes[i].size," bytes",endl;
    }
  public:
    static int get_size(W16 type) {
      //if unlikely (!printed_info) {print_sizes(); printed_info = true;}
      if likely(type == last_type) return last_size;
      // For small (< 60) the linear search is faster...
      last_type = type;
      last_size = sizes[find_lin(type)].size;
      //last_size = sizes[find_chop(type)].size;
      return last_size;
    }
  };

  // I know it is just nasty. We may gain a little by rearranging some of the events
  // NOTE SD: GCC has issues with the PTLsim provided definition of the
  //          offset-of macro together with sizeof
#define FIX_SIZE(T) (__builtin_offsetof(T, start_flexible))
#define FLEX_SIZE(T, F) ((sizeof(T().F)) + (__builtin_offsetof(T, F)))
  const SubfieldSizes::TypeSizeMap SubfieldSizes::sizes[] = {
      {EVENT_FETCH_STALLED,          EVENT_FETCH_OK,            FLEX_SIZE(OutOfOrderCoreEvent, fetch)},
      {EVENT_RENAME_FETCHQ_EMPTY,    EVENT_RENAME_OK,           FLEX_SIZE(OutOfOrderCoreEvent, rename)},
      {EVENT_FRONTEND,               EVENT_FRONTEND,            FLEX_SIZE(OutOfOrderCoreEvent, frontend)},
      {EVENT_CLUSTER_NO_CLUSTER,     EVENT_CLUSTER_OK,          FLEX_SIZE(OutOfOrderCoreEvent, select_cluster)},
      {EVENT_DISPATCH_NO_CLUSTER,    EVENT_DISPATCH_OK,         FLEX_SIZE(OutOfOrderCoreEvent, dispatch)},
      {EVENT_ISSUE_NO_FU,            EVENT_ISSUE_OK,            FLEX_SIZE(OutOfOrderCoreEvent, issue)},
      {EVENT_REPLAY,                 EVENT_REPLAY,              FLEX_SIZE(OutOfOrderCoreEvent, replay)},
      {EVENT_STORE_EXCEPTION,        EVENT_TLBWALK_COMPLETE,    FLEX_SIZE(OutOfOrderCoreEvent, loadstore)},
      {EVENT_FENCE_ISSUED,           EVENT_ALIGNMENT_FIXUP,     FIX_SIZE(OutOfOrderCoreEvent)},
      {EVENT_ANNUL_NO_FUTURE_UOPS,   EVENT_ANNUL_FLUSH,         FLEX_SIZE(OutOfOrderCoreEvent, annul)},
      {EVENT_REDISPATCH_DEPENDENTS,  EVENT_REDISPATCH_EACH_ROB, FLEX_SIZE(OutOfOrderCoreEvent, redispatch)},
      {EVENT_COMPLETE,               EVENT_COMPLETE,            FIX_SIZE(OutOfOrderCoreEvent)},
      {EVENT_BROADCAST,              EVENT_FORWARD,             FLEX_SIZE(OutOfOrderCoreEvent, forwarding)},
      {EVENT_WRITEBACK,              EVENT_WRITEBACK,           FLEX_SIZE(OutOfOrderCoreEvent, writeback)},
      {EVENT_COMMIT_FENCE_COMPLETED, EVENT_COMMIT_OK,           FLEX_SIZE(OutOfOrderCoreEvent, commit)},
      {EVENT_RECLAIM_PHYSREG,        EVENT_RECLAIM_PHYSREG,     FIX_SIZE(OutOfOrderCoreEvent)},
      {EVENT_RELEASE_MEM_LOCK,       EVENT_RELEASE_MEM_LOCK,    FLEX_SIZE(OutOfOrderCoreEvent, loadstore)},
      {EVENT_ASF_ABORT,              EVENT_ASF_ABORT,           FLEX_SIZE(OutOfOrderCoreEvent, abort)},
      {EVENT_ASF_CONFLICT,           EVENT_ASF_CONFLICT,        FLEX_SIZE(OutOfOrderCoreEvent, conflict)},
      {EVENT_META_COREID,            EVENT_META_COREID,         sizeof(MetadataCoreidEvent)},
      {EVENT_ASF_NESTLEVEL,          EVENT_ASF_NESTLEVEL,       FLEX_SIZE(OutOfOrderCoreEvent, nestlevel)},
  };
  const int SubfieldSizes::sizes_elem = sizeof(sizes)/sizeof(sizes[0]);
  W16 SubfieldSizes::last_type = 0xFFFF;
  int SubfieldSizes::last_size = 0;
  bool SubfieldSizes::printed_info = false;

  ostream& FlexibleOOOCoreBinaryEvent::print_binary(ostream& os, W8 coreid) const {
//    os.write(this, sizeof(*this));

    W16 size;
    size = SubfieldSizes::get_size(type) + sizeof(size);
    os.write(&size, sizeof(size));
    const OutOfOrderCoreEvent* p = this;
    os.write(p, size - sizeof(size));

// Use stupid dumping for now.
#if (0)
    bool ld = isload(uop.opcode);
    bool st = isstore(uop.opcode);
    bool br = isbranch(uop.opcode);
    boll rl = (uop.opcode == OP_rel);
    W32 exception = LO32(commit.state.reg.rddata);
    W32 error_code = HI32(commit.state.reg.rddata);

    FlexibleTraceEvent rep;

    // Figure out event type, I know this is nasty.
    if (ld)      rep.hdr.event = FlexibleTraceEventHeader::LOAD;
    else if (st) rep.hdr.event = FlexibleTraceEventHeader::STORE;
    else if (pf) rep.hdr.event = FlexibleTraceEventHeader::PREFETCH;
    else if (rl) rep.hdr.event = FlexibleTraceEventHeader::RELEASE;
    else if (uop.opcode == OP_spec) rep.hdr.event = FlexibleTraceEventHeader::SPECULATE;
    else if (uop.opcode == OP_com)  rep.hdr.event = FlexibleTraceEventHeader::COMMIT;

    switch (type) {
    case EVENT_COMMIT_OK: {
      rep.hdr.pipestage = FlexibleTraceEventHeader::COMMIT_STAGE;
      break;
    }
    case EVENT_DISPATCH_OK: {
      rep.hdr.pipestage = FlexibleTraceEventHeader::DISPATCH_STAGE;
      break;
    }
    default:
      break;
    }
      // Values
      //   uop.rd                  - Destination arch-register
      //   commit.state.reg.rddata - Data in destination arch-register
      //   - Do we need flags?
      //   Core + cycle generated from outer code!
      //   rip                      - Own RIP
      //   uop.eom                  - Destination register is RIP
      //   commit.target_rip        - Target RIP (for branches interesting!)
      //   commit.state.st.physaddr - Physical address for stores
      //   commit.state.st.data     - Data for stores
      //   commit.state.st.bytemask - Bytemask for stores
      //   commit.state.reg.addr    - Physical address for loads
      //   commit.origvirt          - virtual address (incl. byte offset!)
      //   uop.size                 - Size for loads and stores (2^n bytes)
      //   uop.is_asf               - ASF flag for these instructions
      //   uop.opcode               - differenciate between ASF start / end
      //   threadid                 - thread ID
      //   cycle                    - Simulation cycle



      int offset = 0;
      if (ld) {
        rep.var.data     = commit.state.reg.rddata;
        rep.var.length   = (1 << uop.size);
      } else if (st) {
        rep.var.data     = commit.state.st.data;
        rep.var.length   = popcount8bit(commit.state.st.bytemask);
        offset           = lsbindex8bit(commit.state.st.bytemask);
      }
      if (ld|st|pf|rl) {
        rep.var.physaddr = commit.state.reg.addr + offset;
        rep.var.virtaddr = commit.origvirt;
      }
      break;
    }
    }

    // Fill fixed headers
    rep.hdr.coreid   = coreid;
    rep.hdr.threadid = threadid;
    rep.hdr.cycle    = cycle;
    rep.hdr.rip      = rip.rip;
    rep.hdr.length   = foo;

    os.write(&rep, sizeof(rep.hdr));
#endif
    return os;
  }
};

ostream& RegisterRenameTable::print(ostream& os) const {
  foreach (i, TRANSREG_COUNT) {
    if ((i % 8) == 0) os << " ";
    os << " ", padstring(arch_reg_names[i], -6), " r", intstring((*this)[i]->index(), -3), " | ";
    if (((i % 8) == 7) || (i == TRANSREG_COUNT-1)) os << endl;
  }
  return os;
}

//
// Get the thread priority, with lower numbers receiving higher priority.
// This is used to regulate the order in which fetch, rename, frontend
// and dispatch slots are filled in each cycle.
//
// The well known ICOUNT algorithm adds up the number of uops in
// the frontend pipeline stages and gives highest priority to
// the thread with the lowest number, since this thread is moving
// uops through very quickly and can make more progress.
//
int ThreadContext::get_priority() const {
  int priority =
    fetchq.count +
    rob_frontend_list.count +
    rob_ready_to_dispatch_list.count;

  for_each_cluster (cluster) {
    priority +=
      rob_dispatched_list[cluster].count +
      rob_ready_to_issue_list[cluster].count +
      rob_ready_to_store_list[cluster].count +
      rob_ready_to_load_list[cluster].count;
  }

  return priority;
}

//
// Execute one cycle of the entire core state machine
//
bool OutOfOrderCore::runcycle() {
  bool exiting = 0;
  //
  // Detect edge triggered transition from 0->1 for
  // pending interrupt events, then wait for current
  // x86 insn EOM uop to commit before redirecting
  // to the interrupt handler.
  //

#ifdef PTLSIM_HYPERVISOR
  foreach (i, threadcount) {
    ThreadContext* thread = threads[i];
    bool current_interrupts_pending = thread->ctx.check_events();
    bool edge_triggered = ((!thread->prev_interrupts_pending) & current_interrupts_pending);
    thread->handle_interrupt_at_next_eom |= edge_triggered;
    thread->prev_interrupts_pending = current_interrupts_pending;
  }
#endif

  //
  // Compute reserved issue queue entries to avoid starvation:
  //
#ifndef MULTI_IQ
#ifdef ENABLE_CHECKS
  int total_issueq_count = 0;
  int total_issueq_reserved_free = 0;

  foreach (i, MAX_THREADS_PER_CORE) {
    ThreadContext* thread = threads[i];

    if unlikely (!thread) {
      total_issueq_reserved_free += reserved_iq_entries;
    } else {
      total_issueq_count += thread->issueq_count;
      if(thread->issueq_count < reserved_iq_entries){
        total_issueq_reserved_free += reserved_iq_entries - thread->issueq_count;
      }
    }
  }

  assert (total_issueq_count == issueq_all.count);
  assert((ISSUE_QUEUE_SIZE - issueq_all.count) == (issueq_all.shared_entries + total_issueq_reserved_free));
#endif /* ENABLE_CHECKS */
#endif /* MULTI_IQ */

  foreach (i, threadcount) threads[i]->loads_in_this_cycle = 0;

  fu_avail = bitmask(FU_COUNT);
  caches.clock();

  //
  // Backend and issue pipe stages run with round robin priority
  //
  int commitrc[MAX_THREADS_PER_CORE];
  commitcount = 0;
  writecount = 0;

  foreach (permute, threadcount) {
    int tid = add_index_modulo(round_robin_tid, +permute, threadcount);
    ThreadContext* thread = threads[tid];
    if unlikely (!thread->ctx.running) continue;

    commitrc[tid] = thread->commit();
    for_each_cluster(j) thread->writeback(j);
    for_each_cluster(j) thread->transfer(j);
#ifdef ENABLE_ASF
    commitrc[tid] = thread->asf_pipeline_intercept.post_commit(thread->ctx, commitrc[tid]);
#endif
  }

  //
  // Clock the TLB miss page table walk state machine
  // This may use up load ports, so do it before other
  // loads can issue 
  //
#ifdef PTLSIM_HYPERVISOR
  foreach (i, threadcount) {
    threads[i]->tlbwalk();
  }
#endif

  //
  // Always clock the issue queues: they're independent of all threads
  //
  // SD: Moved between forward and issue in the same cycle, so that a 0-cycle
  // forwarding delay would actually be equivalent to a direct bypass!
  foreach_issueq(clock());

  //
  // Issue whatever is ready
  //
  for_each_cluster(i) { issue(i); }

  //
  // Most of the frontend (except fetch!) also works with round robin priority
  //
  int dispatchrc[MAX_THREADS_PER_CORE];
  dispatchcount = 0;
  foreach (permute, threadcount) {
    int tid = add_index_modulo(round_robin_tid, +permute, threadcount);
    ThreadContext* thread = threads[tid];
    if unlikely (!thread->ctx.running) continue;

    for_each_cluster(j) { thread->complete(j); }

    dispatchrc[tid] = thread->dispatch();

    if likely (dispatchrc[tid] >= 0) {
      thread->frontend();
      thread->rename();
    }
  }

  //
  // Compute fetch priorities (default is ICOUNT algorithm)
  //
  // This means we sort in ascending order, with any unused threads
  // (if any) given the lowest priority.
  //

  int priority_value[MAX_THREADS_PER_CORE];
  int priority_index[MAX_THREADS_PER_CORE];

  if likely (threadcount == 1) {
    priority_value[0] = 0;
    priority_index[0] = 0;
  } else {
    foreach (i, threadcount) {
      priority_index[i] = i;
      ThreadContext* thread = threads[i];
      priority_value[i] = thread->get_priority();
      if unlikely (!thread->ctx.running) priority_value[i] = limits<int>::max;
    }
    
    sort(priority_index, threadcount, SortPrecomputedIndexListComparator<int, false>(priority_value));
  }

  //
  // Fetch in thread priority order
  //
  // NOTE: True ICOUNT only fetches the highest priority
  // thread per cycle, since there is usually only one
  // instruction cache port. In a banked i-cache, we can
  // fetch from multiple threads every cycle.
  //
  foreach (j, threadcount) {
    int i = priority_index[j];
    ThreadContext* thread = threads[i];
    assert(thread);
    if unlikely (!thread->ctx.running) {
      continue;
    }

    if likely (dispatchrc[i] >= 0) {
      thread->fetch();
    }
  }

  //
  // Always clock the issue queues: they're independent of all threads
  //
  // SD: Moved between forward and issue in the same cycle, so that a 0-cycle
  // forwarding delay would actually be equivalent to a direct bypass!
  //foreach_issueq(clock());

  //
  // Advance the round robin priority index
  //
  round_robin_tid = add_index_modulo(round_robin_tid, +1, threadcount);

  //
  // Flush event log ring buffer
  //
  if unlikely (config.event_log_enabled) {
    // logfile << "[cycle ", sim_cycle, "] Miss buffer contents:", endl;
    // logfile << caches.missbuf;
    if unlikely (config.flush_event_log_every_cycle) {
      eventlog.flush(true);
    }
  }

#ifdef ENABLE_CHECKS
  // This significantly slows down simulation; only enable it if absolutely needed:
  // check_refcounts();
#endif

  foreach (i, threadcount) {
    ThreadContext* thread = threads[i];
    if unlikely (!thread->ctx.running) continue;
    int rc = commitrc[i];
    if likely ((rc == COMMIT_RESULT_OK) | (rc == COMMIT_RESULT_NONE)) continue;

    switch (rc) {
    case COMMIT_RESULT_SMC: {
      if (logable(3)) logfile << "Potentially cross-modifying SMC detected: global flush required (cycle ", sim_cycle, ", ", total_user_insns_committed, " commits)", endl, flush;
      //
      // DO NOT GLOBALLY FLUSH! It will cut off the other thread(s) in the
      // middle of their currently committing x86 instruction, causing massive
      // internal corruption on any VCPUs that happen to be straddling the
      // instruction boundary.
      //
      // BAD: machine.flush_all_pipelines();
      //
      // This is a temporary fix: in the *extremely* rare case where both
      // threads have the same basic block in their pipelines and that
      // BB is being invalidated, the BB cache will forbid us from
      // freeing it (and will print a warning to that effect).
      //
      // I'm working on a solution to this, to put some BBs on an
      // "invisible" list, where they cannot be looked up anymore,
      // but their memory is not freed until the lock is released.
      //
      foreach (i, threadcount) {
        ThreadContext* t = threads[i];
        if unlikely (!t) continue;
        if (logable(3)) {
          logfile << "  [vcpu ", i, "] current_basic_block = ", t->current_basic_block;  ": ";
          if (t->current_basic_block) logfile << t->current_basic_block->rip;
          logfile << endl;
        }
      }

      assert(thread->flush_pipeline());
      thread->invalidate_smc();
      break;
    }
    case COMMIT_RESULT_EXCEPTION: {
      exiting = !thread->handle_exception();
      break;
    }
    case COMMIT_RESULT_BARRIER: {
      exiting = !thread->handle_barrier();
      break;
    }
    case COMMIT_RESULT_INTERRUPT: {
      thread->handle_interrupt();
      break;
    }
    case COMMIT_RESULT_STOP: {
      assert(thread->flush_pipeline());
      thread->stall_frontend = 1;
      machine.stopped[thread->ctx.vcpuid] = 1;
      // Wait for other cores to sync up, so don't exit right away
      break;
    }
    case COMMIT_RESULT_OK_FLUSH: {
      if (logable(5))
        logfile << "[vcpu ", thread->ctx.vcpuid,"]"__FILE__,":",__LINE__,"@",sim_cycle,
          ": Flushing the pipeline.", endl, flush;

      assert(thread->flush_pipeline());
      break;
    }
    }
  }

#ifdef PTLSIM_HYPERVISOR
  if unlikely (vcpu_online_map_changed) {
    vcpu_online_map_changed = 0;
    foreach (i, contextcount) {
      Context& vctx = contextof(i);
      if likely (!vctx.dirty) continue;
      //
      // The VCPU is coming up for the first time after booting or being
      // taken offline by the user.
      //
      // Force the active core model to flush any cached (uninitialized)
      // internal state (like register file copies) it might have, since
      // it did not know anything about this VCPU prior to now: if it
      // suddenly gets marked as running without this, the core model
      // will try to execute from bogus state data.
      //
      logfile << "VCPU ", vctx.vcpuid, " context was dirty: update core model internal state", endl;

      ThreadContext* tc = threads[vctx.vcpuid];
      assert(tc);
      if unlikely (&tc->ctx != &vctx) {
        logfile << "Context mismatch detected:",endl;
        logfile << "  &contextof(",i,")= ", &vctx, " &threads[", vctx.vcpuid,
          "]->ctx= ",&tc->ctx, endl;
        logfile << "  i= ", i, " contextof(",i,").vcpuid= ", vctx.vcpuid,
          " threads[", vctx.vcpuid, "]->threadid= ", tc->threadid,
          " threads[", vctx.vcpuid, "]->core.coreid= ", tc->core.coreid, endl;

        logfile << "Mismatching contexts:", endl;
        logfile << "  contextof(",i,"):", vctx, endl;
        if (&tc->ctx)
          logfile << "  threads[",vctx.vcpuid,"]->ctx= ", tc->ctx, endl;

        logfile << "Looping through all other contexts:", endl;
        foreach (j, contextcount) {
          Context& vctx     = contextof(j);             // NOTE: These hide
          ThreadContext* tc = threads[vctx.vcpuid];     //       the outer ones!
          logfile << "  j= ", j, " contextof(",j,").vcpuid= ", vctx.vcpuid,
            " threads[", vctx.vcpuid, "]->threadid= ", tc->threadid,
            " threads[", vctx.vcpuid, "]->core.coreid= ", tc->core.coreid, endl;
          logfile << "  &contextof(",j,")= ", &vctx, " &threads[", vctx.vcpuid,
            "]->ctx= ",&tc->ctx, endl;
        }
      }
      assert(&tc->ctx == &vctx);
      assert(tc->flush_pipeline());
      vctx.dirty = 0;
    }
  }
#endif

  foreach (i, threadcount) {
    ThreadContext* thread = threads[i];
    if unlikely (!thread->ctx.running) break;

    if unlikely ((sim_cycle - thread->last_commit_at_cycle) > 1024 * MAX_SIMULATED_VCPUS) {
      stringbuf sb;
      sb << "[vcpu ", thread->ctx.vcpuid, "] thread ", thread->threadid, ": WARNING: At cycle ",
        sim_cycle, ", ", total_user_insns_committed,  " user commits: no instructions have committed for ",
        (sim_cycle - thread->last_commit_at_cycle), " cycles; the pipeline could be deadlocked", endl;
      logfile << sb, flush;
      cerr << sb, flush;
      logfile << thread->ROB, endl, flush;
      exiting = 1;
    }
  }

  return exiting;
}

//
// ReorderBufferEntry
//
void ReorderBufferEntry::init(int idx) {
  this->idx = idx;
  entry_valid = 0;
  selfqueuelink::reset();
  current_state_list = null;
  reset();
}

//
// Clean out various fields from the ROB entry that are 
// expected to be zero when allocating a new ROB entry.
//
void ReorderBufferEntry::reset() {
  int latency, operand;
  // Deallocate ROB entry
  entry_valid = false;
  cycles_left = 0;
  physreg = (PhysicalRegister*)null;
  lfrqslot = -1;
  lsq = 0;
  load_store_second_phase = 0;
  lock_acquired = 0;
  consumer_count = 0;
  executable_on_cluster_mask = 0;
  pteupdate = 0;
  cluster = -1;
#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
  dest_renamed_before_writeback = 0;
  no_branches_between_renamings = 0;
#endif
  issued = 0;
#ifdef ENABLE_ASF
  llbline = (LLBLine*)null;
#endif
}

bool ReorderBufferEntry::ready_to_issue() const {
  bool raready = operands[0]->ready();
  bool rbready = operands[1]->ready();
  bool rcready = operands[2]->ready();
  bool rsready = operands[3]->ready();
  
  if (isstore(uop.opcode)) {
    return (load_store_second_phase) ? (raready & rbready & rcready & rsready) : (raready & rbready);
  } else if (isload(uop.opcode)) {
    return (load_store_second_phase) ? (raready & rbready & rcready & rsready) : (raready & rbready & rcready);
  } else {
    return (raready & rbready & rcready & rsready);
  }
}

bool ReorderBufferEntry::ready_to_commit() const {
  return (current_state_list == &getthread().rob_ready_to_commit_queue);
}

StateList& ReorderBufferEntry::get_ready_to_issue_list() const {
  OutOfOrderCore& core = getcore();
  ThreadContext& thread = getthread();
  return 
    isload(uop.opcode) ? thread.rob_ready_to_load_list[cluster] :
    isstore(uop.opcode) ? thread.rob_ready_to_store_list[cluster] :
    thread.rob_ready_to_issue_list[cluster];
}

//
// Reorder Buffer
//
stringbuf& ReorderBufferEntry::get_operand_info(stringbuf& sb, int operand) const {
  PhysicalRegister& physreg = *operands[operand];
  ReorderBufferEntry& sourcerob = *physreg.rob;

  sb << "r", physreg.index();
  if (PHYS_REG_FILE_COUNT > 1) sb << "@", getcore().physregfiles[physreg.rfid].name;

  switch (physreg.state) {
  case PHYSREG_WRITTEN:
    sb << " (written)"; break;
  case PHYSREG_BYPASS:
    sb << " (ready)"; break;
  case PHYSREG_WAITING:
    sb << " (wait rob ", sourcerob.index(), " uuid ", sourcerob.uop.uuid, ")"; break;
  case PHYSREG_ARCH: break;
    if (physreg.index() == PHYS_REG_NULL)  sb << " (zero)"; else sb << " (arch ", arch_reg_names[physreg.archreg], ")"; break;
  case PHYSREG_PENDINGFREE:
    sb << " (pending free for ", arch_reg_names[physreg.archreg], ")"; break;
  default:
    // Cannot be in free state!
    sb << " (FREE)"; break;
  }

  return sb;
}

ThreadContext& ReorderBufferEntry::getthread() const { return *getcore().threads[threadid]; }

issueq_tag_t ReorderBufferEntry::get_tag() {
  int mask = ((1 << MAX_THREADS_BIT) - 1) << MAX_ROB_IDX_BIT;
  if (logable(100)) logfile << " get_tag() thread ", (void*) threadid, " rob idx ", (void*)idx, " mask ", (void*)mask, endl;

  assert(!(idx & mask)); 
  assert(!(threadid >> MAX_THREADS_BIT));
  //  int threadid = 1;  
  issueq_tag_t rc = (idx | (threadid << MAX_ROB_IDX_BIT));
  if (logable(100)) logfile <<  " tag ", (void*) rc, endl;
  return rc;
}

ostream& ReorderBufferEntry::print_operand_info(ostream& os, int operand) const {
  stringbuf sb;
  get_operand_info(sb, operand);
  os << sb;
  return os;
}

ostream& ReorderBufferEntry::print(ostream& os) const {
  stringbuf name, rainfo, rbinfo, rcinfo;
  nameof(name, uop);
  get_operand_info(rainfo, 0);
  get_operand_info(rbinfo, 1);
  get_operand_info(rcinfo, 2);

  os << "rob ", intstring(index(), -3), " uuid ", intstring(uop.uuid, 16), " rip 0x", hexstring(uop.rip, 48), " ",
    padstring(current_state_list->name, -24), " ", (uop.som ? "SOM" : "   "), " ", (uop.eom ? "EOM" : "   "), 
    " @ ", padstring((cluster >= 0) ? clusters[cluster].name : "???", -4), " ",
    padstring(name, -12), " r", intstring(physreg->index(), -3), " ", padstring(arch_reg_names[uop.rd], -6);
  if (isload(uop.opcode)) 
    os << " ld", intstring(lsq->index(), -3);
  else if (isstore(uop.opcode))
    os << " st", intstring(lsq->index(), -3);
  else os << "      ";

  os << " = ";
  os << padstring(rainfo, -30);
  os << padstring(rbinfo, -30);
  os << padstring(rcinfo, -30);

#ifdef ENABLE_ASF
  if (llbline)
    os << " llb: ", llbline;
#endif

  return os;
}

void ThreadContext::print_rob(ostream& os) {
  os << "ROB head ", ROB.head, " to tail ", ROB.tail, " (", ROB.count, " entries):", endl;
  foreach_forward(ROB, i) {
    ReorderBufferEntry& rob = ROB[i];
    os << "  ", rob, endl;
  }
}

void ThreadContext::print_lsq(ostream& os) {
  os << "LSQ head ", LSQ.head, " to tail ", LSQ.tail, " (", LSQ.count, " entries):", endl;
  foreach_forward(LSQ, i) {
    LoadStoreQueueEntry& lsq = LSQ[i];
    os << "  ", lsq, endl;
  }
}

void ThreadContext::print_rename_tables(ostream& os) {
  os << "SpecRRT:", endl;
  os << specrrt;
  os << "CommitRRT:", endl;
  os << commitrrt;
}

void OutOfOrderCore::print_smt_state(ostream& os) {
  os << "Print SMT statistics:", endl;

  foreach (i, threadcount) {
    ThreadContext* thread = threads[i];
    os << "Thread ", i, ":", endl,
      "  total_uops_committed ", thread->total_uops_committed, endl,
      "  uipc ", double(thread->total_uops_committed) / double(iterations), endl,
      "  total_insns_committed ",  thread->total_insns_committed,
      "  ipc ", double(thread->total_insns_committed) / double(iterations), endl;
  }
}

void ThreadContext::dump_smt_state(ostream& os) {
  os << "SMT per-thread state for t", threadid, ":", endl;

  print_rename_tables(os);
  print_rob(os);
  print_lsq(os);
  os << flush;
}

void OutOfOrderCore::dump_smt_state(ostream& os) {
  os << "SMT common structures:", endl;

  print_list_of_state_lists<PhysicalRegister>(os, physreg_states, "Physical register states");
  foreach (i, PHYS_REG_FILE_COUNT) {
    os << physregfiles[i];
  }

  print_list_of_state_lists<ReorderBufferEntry>(os, rob_states, "ROB entry states");
  os << "Issue Queues:", endl;
  foreach_issueq(print(os));
  caches.print(os);

  os << "Unaligned predictor:", endl;
  os << "  ", unaligned_predictor.popcount(), " unaligned bits out of ", UNALIGNED_PREDICTOR_SIZE, " bits", endl;
  os << "  Raw data: ", unaligned_predictor, endl;

  foreach (i, threadcount) {
    ThreadContext* thread = threads[i];
    thread->dump_smt_state(os);
  }
}

//
// Validate the physical register reference counters against what
// is really accessible from the various tables and operand fields.
//
// This is for debugging only.
//
void OutOfOrderCore::check_refcounts() {
  // this should be for each thread instead of whole core:
  // for now, we just work on thread[0];
  ThreadContext& thread = *threads[0];
  Queue<ReorderBufferEntry, ROB_SIZE>& ROB = thread.ROB;
  RegisterRenameTable& specrrt = thread.specrrt;
  RegisterRenameTable& commitrrt = thread.commitrrt;

  int refcounts[PHYS_REG_FILE_COUNT][MAX_PHYS_REG_FILE_SIZE];
  memset(refcounts, 0, sizeof(refcounts));

  foreach (rfid, PHYS_REG_FILE_COUNT) {
    // Null physreg in each register file is special and can never be freed:
    refcounts[rfid][PHYS_REG_NULL]++;
  }

  foreach_forward(ROB, i) {
    ReorderBufferEntry& rob = ROB[i];
    foreach (j, MAX_OPERANDS) {
      refcounts[rob.operands[j]->rfid][rob.operands[j]->index()]++;
    }
  }

  foreach (i, TRANSREG_COUNT) {
    refcounts[commitrrt[i]->rfid][commitrrt[i]->index()]++;
    refcounts[specrrt[i]->rfid][specrrt[i]->index()]++;
  }

  bool errors = 0;

  foreach (rfid, PHYS_REG_FILE_COUNT) {
    PhysicalRegisterFile& physregs = physregfiles[rfid];
    foreach (i, physregs.size) {
      if unlikely (physregs[i].refcount != refcounts[rfid][i]) {
        logfile << "ERROR: r", i, " refcount is ", physregs[i].refcount, " but should be ", refcounts[rfid][i], endl;
        
        foreach_forward(ROB, r) {
          ReorderBufferEntry& rob = ROB[r];
          foreach (j, MAX_OPERANDS) {
            if ((rob.operands[j]->index() == i) & (rob.operands[j]->rfid == rfid)) logfile << "  ROB ", r, " operand ", j, endl;
          }
        }
        
        foreach (j, TRANSREG_COUNT) {
          if ((commitrrt[j]->index() == i) & (commitrrt[j]->rfid == rfid)) logfile << "  CommitRRT ", arch_reg_names[j], endl;
          if ((specrrt[j]->index() == i) & (specrrt[j]->rfid == rfid)) logfile << "  SpecRRT ", arch_reg_names[j], endl;
        }
        
        errors = 1;
      }
    }
  }

  if (errors) assert(false);
}

void OutOfOrderCore::check_rob() {
  // this should be for each thread instead of whole core:
  // for now, we just work on thread[0];
  ThreadContext& thread = *threads[0];
  Queue<ReorderBufferEntry, ROB_SIZE>& ROB = thread.ROB;

  foreach (i, ROB_SIZE) {
    ReorderBufferEntry& rob = ROB[i];
    if (!rob.entry_valid) continue;
    assert(inrange((int)rob.forward_cycle, 0, (MAX_FORWARDING_LATENCY+1)-1));
  }

  foreach (i, threadcount) {
    ThreadContext* thread = threads[i];
    foreach (i, rob_states.count) {
      StateList& list = *(thread->rob_states[i]);
      ReorderBufferEntry* rob;
      foreach_list_mutable(list, rob, entry, nextentry) {
        assert(inrange(rob->index(), 0, ROB_SIZE-1));
        assert(rob->current_state_list == &list);
        if (!((rob->current_state_list != &thread->rob_free_list) ? rob->entry_valid : (!rob->entry_valid))) {
          logfile << "ROB ", rob->index(), " list = ", rob->current_state_list->name, " entry_valid ", rob->entry_valid, endl, flush;
          dump_smt_state(logfile);
        assert(false);
        }
      }
    }
  }
}

ostream& LoadStoreQueueEntry::print(ostream& os) const {
  os << (store ? "st" : "ld"), intstring(index(), -3), " ";
  os << "uuid ", intstring(rob->uop.uuid, 10), " ";
  os << "rob ", intstring(rob->index(), -3), " ";
  os << "r", intstring(rob->physreg->index(), -3);
  if (PHYS_REG_FILE_COUNT > 1) os << "@", getcore().physregfiles[rob->physreg->rfid].name;
  os << " ";
  if (invalid) {
    os << "< Invalid: fault 0x", hexstring(data, 8), " > ";
  } else {
    if (datavalid)
      os << bytemaskstring((const byte*)&data, bytemask, 8);
    else os << "<    Data Invalid     >";
    os << " @ ";
    if (addrvalid)
      os << "0x", hexstring(physaddr << 3, 48);
    else os << "< Addr Inval >";
  }    
  return os;
}

//
// Barriers must flush the fetchq and stall the frontend until
// after the barrier is consumed. Execution resumes at the address
// in internal register nextrip (rip after the instruction) after
// handling the barrier in microcode.
//
bool ThreadContext::handle_barrier() {
  // Release resources of everything in the pipeline:

  core_to_external_state();
  assert(flush_pipeline());

  int assistid = ctx.commitarf[REG_rip];
  assist_func_t assist = (assist_func_t)(Waddr)assistid_to_func[assistid];
  
  // SD: Print event log for undefined opcodes.
  if unlikely (assistid == ASSIST_INVALID_OPCODE) {
    if unlikely (config.event_log_enabled)
      getcore().eventlog.print(logfile);
  }

  if (logable(4)) {
    logfile << "[vcpu ", ctx.vcpuid, "] Barrier (#", assistid, " -> ", (void*)assist, " ", assist_name(assist), " called from ",
      (RIPVirtPhys(ctx.commitarf[REG_selfrip]).update(ctx)), "; return to ", (void*)(Waddr)ctx.commitarf[REG_nextrip],
      ") at ", sim_cycle, " cycles, ", total_user_insns_committed, " commits", endl, flush;
  }
  
  if (logable(6)) logfile << "Calling assist function at ", (void*)assist, "...", endl, flush; 
  
  update_assist_stats(assist);
  if (logable(6)) {
    logfile << "Before assist:", endl, ctx, endl;
#ifdef PTLSIM_HYPERVISOR
    logfile << sshinfo, endl;
#endif
  }
  
  assist(ctx);
  
  if (logable(6)) {
    logfile << "Done with assist", endl;
    logfile << "New state:", endl;
    logfile << ctx;
#ifdef PTLSIM_HYPERVISOR
    logfile << sshinfo;
#endif
  }

  // Flush again, but restart at possibly modified rip
  assert(flush_pipeline());

#ifndef PTLSIM_HYPERVISOR
  if (requested_switch_to_native) {
    logfile << "PTL call requested switch to native mode at rip ", (void*)(Waddr)ctx.commitarf[REG_rip], endl;
    return false;
  }
#endif
  return true;
}

bool ThreadContext::handle_exception() {
  // Release resources of everything in the pipeline:
  core_to_external_state();
  assert(flush_pipeline());

  if (logable(4)) {
    logfile << "[vcpu ", ctx.vcpuid, "] Exception ", exception_name(ctx.exception), " called from rip ", (void*)(Waddr)ctx.commitarf[REG_rip], 
      " at ", sim_cycle, " cycles, ", total_user_insns_committed, " commits", endl, flush;
  }

  //
  // CheckFailed and SkipBlock exceptions are raised by the chk uop.
  // This uop is used at the start of microcoded instructions to assert
  // that certain conditions are true so complex corrective actions can
  // be taken if the check fails.
  //
  // SkipBlock is a special case used for checks at the top of REP loops.
  // Specifically, if the %rcx register is zero on entry to the REP, no
  // action at all is to be taken; the rip should simply advance to
  // whatever is in chk_recovery_rip and execution should resume.
  //
  // CheckFailed exceptions usually indicate the processor needs to take
  // evasive action to avoid a user visible exception. For instance, 
  // CheckFailed is raised when an inlined floating point operand is
  // denormal or otherwise cannot be handled by inlined fastpath uops,
  // or when some unexpected segmentation or page table conditions
  // arise.
  //
  if (ctx.exception == EXCEPTION_SkipBlock) {
    ctx.commitarf[REG_rip] = chk_recovery_rip;
    if (logable(6)) logfile << "SkipBlock pseudo-exception: skipping to ", (void*)(Waddr)ctx.commitarf[REG_rip], endl, flush;
    assert(flush_pipeline());
    return true;
  }

#ifdef PTLSIM_HYPERVISOR
  //
  // Map PTL internal hardware exceptions to their x86 equivalents,
  // depending on the context. The error_code field should already
  // be filled out.
  //
  // Exceptions not listed here are propagated by microcode
  // rather than the processor itself.
  //
  switch (ctx.exception) {
  case EXCEPTION_PageFaultOnRead:
  case EXCEPTION_PageFaultOnWrite:
  case EXCEPTION_PageFaultOnExec:
    ctx.x86_exception = EXCEPTION_x86_page_fault; break;
  case EXCEPTION_FloatingPointNotAvailable:
    ctx.x86_exception = EXCEPTION_x86_fpu_not_avail; break;
  case EXCEPTION_FloatingPoint:
    ctx.x86_exception = EXCEPTION_x86_fpu; break;
  case EXCEPTION_ASF_Abort:
    // SD NOTE: These set the proper x86_exception field already in asf.cpp!
    break;
  default:
    logfile << "Unsupported internal exception type ", exception_name(ctx.exception), endl, flush;
    assert(false);
  }

  if (logable(4)) {
    logfile << ctx;
    logfile << sshinfo;
  }

  ctx.propagate_x86_exception(ctx.x86_exception, ctx.error_code, ctx.cr2);

  // Flush again, but restart at modified rip
  assert(flush_pipeline());

  return true;
#else
  if (logable(6)) 
    logfile << "Exception (", exception_name(ctx.exception), " called from ", (void*)(Waddr)ctx.commitarf[REG_rip], 
      ") at ", sim_cycle, " cycles, ", total_user_insns_committed, " commits", endl, flush;

  stringbuf sb;
  logfile << exception_name(ctx.exception), " detected at fault rip ", (void*)(Waddr)ctx.commitarf[REG_rip], " @ ", 
    total_user_insns_committed, " commits (", total_uops_committed, " uops): genuine user exception (",
    exception_name(ctx.exception), "); aborting", endl;
  logfile << ctx, endl;
  logfile << flush;

  logfile << "Aborting...", endl, flush;
  cerr << "Aborting...", endl, flush;

  assert(false);
  return false;
#endif
}

bool ThreadContext::handle_interrupt() {
#ifdef PTLSIM_HYPERVISOR
  // Release resources of everything in the pipeline:
  core_to_external_state();
  assert(flush_pipeline());

  if (logable(6)) {
    logfile << "[vcpu ", threadid, "] interrupts pending at ", sim_cycle, " cycles, ", total_user_insns_committed, " commits", endl, flush;
    logfile << "Context at interrupt:", endl;
    logfile << ctx;
    logfile << sshinfo;
    logfile.flush();
  }

  ctx.event_upcall();

  if (logable(6)) {
    logfile <<  "[vcpu ", threadid, "] after interrupt redirect:", endl;
    logfile << ctx;
    logfile << sshinfo;
    logfile.flush();
  }

  // Flush again, but restart at modified rip
  assert(flush_pipeline());
#endif
  return true;
}

//
// Event Formatting
//
void PhysicalRegister::fill_operand_info(PhysicalRegisterOperandInfo& opinfo) {
  opinfo.physreg = index();
  opinfo.state = state;
  opinfo.rfid = rfid;
  opinfo.archreg = archreg;
  if (rob) {
    opinfo.rob = rob->index();
    opinfo.uuid = rob->uop.uuid;
  }
}

ostream& OutOfOrderModel::operator <<(ostream& os, const PhysicalRegisterOperandInfo& opinfo) {
  os << "[r", opinfo.physreg, " ", short_physreg_state_names[opinfo.state], " ";
  switch (opinfo.state) {
  case PHYSREG_WAITING:
  case PHYSREG_BYPASS:
  case PHYSREG_WRITTEN:
    os << "rob ", opinfo.rob, " uuid ", opinfo.uuid; break;
  case PHYSREG_ARCH:
  case PHYSREG_PENDINGFREE:
    os << arch_reg_names[opinfo.archreg]; break;
  };
  os << "]";
  return os;
}

OutOfOrderMachine::OutOfOrderMachine(const char* name) {
  // Add to the list of available core types
  addmachine(name, this);
}

//
// Construct all the structures necessary to configure
// the cores. This function is only called once, after
// all other PTLsim subsystems are brought up.
//

bool OutOfOrderMachine::init(PTLsimConfig& config) {
#ifdef ENABLE_SMT
  // Note: we only create a single core for all contexts for now.
  cores[0] = new OutOfOrderCore(0, *this);
  corecount = 1;
#else
  corecount = 0;
#endif

  foreach (i, contextcount) {
#ifdef ENABLE_SMT
    OutOfOrderCore& core = *cores[0];
#else
    cores[corecount] = new OutOfOrderCore(corecount, *this);
    OutOfOrderCore& core    = *cores[corecount];
    corecount++;
#endif

    ThreadContext* thread = new ThreadContext(core, core.threadcount, contextof(i));
    core.threads[core.threadcount] = thread;
    core.threadcount++;
    thread->init();
    logfile << "New ThreadContext: Core ", core.coreid, " (", core.threadcount," threads) Thread ", thread->threadid, " &thread=", thread, " &core=", &core, corecount, " total cores ", corecount, endl, flush;
    //
    // Note: in a multi-processor model, config may
    // specify various ways of slicing contextcount up
    // into threads, cores and sockets; the appropriate
    // interconnect and cache hierarchy parameters may
    // be specified here.
    //
  }

  foreach (i, corecount) cores[i]->init();
  init_luts();

  return true;
}

//
// Run the processor model, until a stopping point
// is hit (as configured elsewhere in config).
//
int OutOfOrderMachine::run(PTLsimConfig& config) {
  time_this_scope(cttotal);

  logfile << "Starting out-of-order core toplevel loop", endl, flush;

  // All VCPUs are running:
  stopped = 0;

  if unlikely (iterations >= config.start_log_at_iteration) {
    if unlikely (!logenable) logfile << "Start logging at level ", config.loglevel, " in cycle ", iterations, endl, flush;
    logenable = 1;
  }

  foreach (i, corecount) {
    cores[i]->reset();
    cores[i]->flush_pipeline_all();


  logfile << "IssueQueue states:", endl;

    if unlikely (config.event_log_enabled && (!cores[i]->eventlog.start)) {
      cores[i]->eventlog.init(config.event_log_ring_buffer_size);
      cores[i]->eventlog.logfile = &eventlogfile;
    }
  }

  bool exiting = false;
  bool stopping = false;

  for (;;) {
    if unlikely (iterations >= config.start_log_at_iteration) {
      if unlikely (!logenable) logfile << "Start logging at level ", config.loglevel, " in cycle ", iterations, endl, flush;
      logenable = 1;
    }

    update_progress();
    inject_events();

    int running_thread_count = 0;
    foreach (j, corecount) {
      OutOfOrderCore& core =* cores[j];
    foreach (i, core.threadcount) {
      ThreadContext* thread = core.threads[i];
#ifdef PTLSIM_HYPERVISOR
      running_thread_count += thread->ctx.running;
      if unlikely (!thread->ctx.running) {
        if unlikely (stopping) {
          // Thread is already waiting for an event: stop it now
          logfile << "[vcpu ", thread->ctx.vcpuid, "] Already stopped at cycle ", sim_cycle, endl;
          stopped[thread->ctx.vcpuid] = 1;
        } else {
          if (thread->ctx.check_events()) thread->handle_interrupt();
        }
          continue; /* NB, SD: Back to foreach (i, core.threadcount), that doesn't make much sense in the original impl, either! */
      }
#endif
    }

    exiting |= core.runcycle();
    }

    if unlikely (check_for_async_sim_break() && (!stopping)) {
      logfile << "Waiting for all VCPUs to reach stopping point, starting at cycle ", sim_cycle, endl;
      // force_logging_enabled();
      foreach (j, corecount) {
        OutOfOrderCore& core = *cores[j];
      foreach (i, core.threadcount) core.threads[i]->stop_at_next_eom = 1;
      }
      if (config.abort_at_end) {
        config.abort_at_end = 0;
        logfile << "Abort immediately: do not wait for next x86 boundary nor flush pipelines", endl;
        stopped = 1;
        exiting = 1;
      }
      stopping = 1;
    }

    stats.summary.cycles++;
    stats.ooocore.cycles++;
    sim_cycle++;
    unhalted_cycle_count += (running_thread_count > 0);
    iterations++;

    if unlikely (stopping) {
      // logfile << "Waiting for all VCPUs to stop at ", sim_cycle, ": mask = ", stopped, " (need ", contextcount, " VCPUs)", endl;
      exiting |= (stopped.integer() == bitmask(contextcount));
    }

    if unlikely (exiting) break;
  }

  logfile << "Exiting out-of-order core at ", total_user_insns_committed, " commits, ", total_uops_committed, " uops and ", iterations, " iterations (cycles)", endl;

  foreach (j, corecount) {
    OutOfOrderCore& core =* cores[j];

  foreach (i, core.threadcount) {
    ThreadContext* thread = core.threads[i];

    thread->core_to_external_state();

    if (logable(6) | ((sim_cycle - thread->last_commit_at_cycle) > 1024) | config.dump_state_now) {
      logfile << "Core State at end for thread ", thread->threadid, ": ", endl;
      logfile << thread->ctx;
    }
  }
  }

  config.dump_state_now = 0;

  dump_state(logfile);
  
  // Flush everything to remove any remaining refs to basic blocks
  flush_all_pipelines();

  return exiting;
}

void OutOfOrderCore::flush_tlb(Context& ctx, int threadid, bool selective, Waddr virtaddr) {
  ThreadContext& thread =* threads[threadid];
  if (logable(5)) {
    logfile << "[vcpu ", ctx.vcpuid, "] core ", coreid, ", thread ", threadid, ": Flush TLBs";
    if (selective) logfile << " for virtaddr ", (void*)virtaddr, endl;
    logfile << endl;
    //logfile << "DTLB before: ", endl, caches.dtlb, endl;
    //logfile << "ITLB before: ", endl, caches.itlb, endl;
  }
  int dn; int in;

  if unlikely (selective) {
    dn = caches.dtlb.flush_virt(virtaddr, threadid);
    in = caches.itlb.flush_virt(virtaddr, threadid);
#ifdef USE_L2_TLB
    dn += caches.l2dtlb.flush_virt(virtaddr, threadid);
    in += caches.l2itlb.flush_virt(virtaddr, threadid);
#endif
  } else {
    dn = caches.dtlb.flush_thread(threadid);
    in = caches.itlb.flush_thread(threadid);
#ifdef USE_L2_TLB
    dn += caches.l2dtlb.flush_thread(threadid);
    in += caches.l2itlb.flush_thread(threadid);
#endif
  }
  if (logable(5)) {
    logfile << "Flushed ", dn, " DTLB slots and ", in, " ITLB slots", endl;
    //logfile << "DTLB after: ", endl, caches.dtlb, endl;
    //logfile << "ITLB after: ", endl, caches.itlb, endl;
  }
}

void OutOfOrderMachine::flush_tlb(Context& ctx) {
  // This assumes all VCPUs are mapped statically to either cores or threads in a single SMT core
#ifdef ENABLE_SMT
  int coreid = 0;
  int threadid = ctx.vcpuid;
#else
  int coreid = ctx.vcpuid;;
  int threadid = 0;
#endif
  cores[coreid]->flush_tlb(ctx, threadid);
}

void OutOfOrderMachine::flush_tlb_virt(Context& ctx, Waddr virtaddr) {
  // This assumes all VCPUs are mapped statically to either cores or threads in a single SMT core
#ifdef ENABLE_SMT
  int coreid = 0;
  int threadid = ctx.vcpuid;
#else
  int coreid = ctx.vcpuid;;
  int threadid = 0;
#endif
  cores[coreid]->flush_tlb(ctx, threadid, true, virtaddr);
}

void OutOfOrderMachine::dump_state(ostream& os) {
  os << " dump_state include event if -ringbuf enabled: ",endl;
  //  foreach (i, contextcount) {
  foreach (i, MAX_SMT_CORES) {
    os << " dump_state for core ", i,endl,flush;
    if (!cores[i]) continue;
    OutOfOrderCore& core =* cores[i];
    Context& ctx = contextof(i);
    if unlikely (config.event_log_enabled) 
                  core.eventlog.print(logfile);
    else
      os << " config.event_log_enabled is not enabled ", config.event_log_enabled, endl;
    
    core.dump_smt_state(os);
    core.print_smt_state(os);
  }
  os << "Memory interlock buffer:", endl, flush;
  interlocks.print(os);
#if 0
  //
  // For debugging only:
  //
  foreach (i, threadcount) {
    ThreadContext* thread = *cores[0]->threads[i];
    os << "Thread ", i, ":", endl;
    os << "  rip:                                 ", (void*)thread.ctx.commitarf[REG_rip], endl;
    os << "  consecutive_commits_inside_spinlock: ", thread.consecutive_commits_inside_spinlock, endl;
    os << "  State:", endl;
    os << thread.ctx;
  }
#endif
}

namespace OutOfOrderModel {
  CycleTimer cttotal;
  CycleTimer ctfetch;
  CycleTimer ctdecode;
  CycleTimer ctrename;
  CycleTimer ctfrontend;
  CycleTimer ctdispatch;
  CycleTimer ctissue;
  CycleTimer ctissueload;
  CycleTimer ctissuestore;
  CycleTimer ctcomplete;
  CycleTimer cttransfer;
  CycleTimer ctwriteback;
  CycleTimer ctcommit;
};

void OutOfOrderMachine::update_stats(PTLsimStats& stats) {
  foreach (vcpuid, contextcount) {
    PerContextOutOfOrderCoreStats& s = per_context_ooocore_stats_ref(vcpuid);
    s.issue.uipc = s.issue.uops / (double)stats.ooocore.cycles;
    s.commit.uipc = (double)s.commit.uops / (double)stats.ooocore.cycles;
    s.commit.ipc = (double)s.commit.insns / (double)stats.ooocore.cycles;
  }

  PerContextOutOfOrderCoreStats& s = stats.ooocore.total;
  s.issue.uipc = s.issue.uops / (double)stats.ooocore.cycles;
  s.commit.uipc = (double)s.commit.uops / (double)stats.ooocore.cycles;
  s.commit.ipc = (double)s.commit.insns / (double)stats.ooocore.cycles;

  stats.ooocore.simulator.total_time = cttotal.seconds();
  stats.ooocore.simulator.cputime.fetch = ctfetch.seconds();
  stats.ooocore.simulator.cputime.decode = ctdecode.seconds();
  stats.ooocore.simulator.cputime.rename = ctrename.seconds();
  stats.ooocore.simulator.cputime.frontend = ctfrontend.seconds();
  stats.ooocore.simulator.cputime.dispatch = ctdispatch.seconds();
  stats.ooocore.simulator.cputime.issue = ctissue.seconds() - (ctissueload.seconds() + ctissuestore.seconds());
  stats.ooocore.simulator.cputime.issueload = ctissueload.seconds();
  stats.ooocore.simulator.cputime.issuestore = ctissuestore.seconds();
  stats.ooocore.simulator.cputime.complete = ctcomplete.seconds();
  stats.ooocore.simulator.cputime.transfer = cttransfer.seconds();
  stats.ooocore.simulator.cputime.writeback = ctwriteback.seconds();
  stats.ooocore.simulator.cputime.commit = ctcommit.seconds();
}

//
// Flush all pipelines in every core, and process any
// pending BB cache invalidates.
//
// Typically this is in response to some infrequent event
// like cross-modifying SMC or cache coherence deadlocks.
//
void OutOfOrderMachine::flush_all_pipelines() {
  assert(cores[0]);
  OutOfOrderCore* core = cores[0];

  //
  // Make sure all pipelines are flushed BEFORE
  // we try to invalidate the dirty page!
  // Otherwise there will still be some remaining
  // references to to the basic block
  //
  core->flush_pipeline_all();

  foreach (i, core->threadcount) {
    ThreadContext* thread = core->threads[i];
    thread->invalidate_smc();
  }  
}

/* We have asf now! */
OutOfOrderMachine ooomodel("asfooo");

OutOfOrderCore& OutOfOrderModel::coreof(int coreid) {
  return *ooomodel.cores[coreid];
}
