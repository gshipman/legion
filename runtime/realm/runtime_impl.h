/* Copyright 2015 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Runtime implementation for Realm

#ifndef REALM_RUNTIME_IMPL_H
#define REALM_RUNTIME_IMPL_H

#include "runtime.h"
#include "id.h"

#include "activemsg.h"
#include "operation.h"
#include "profiling.h"

#include "dynamic_table.h"
#include "proc_impl.h"

// event and reservation impls are included directly in the node's dynamic tables,
//  so we need their definitions here (not just declarations)
#include "event_impl.h"
#include "rsrv_impl.h"

#include "machine_impl.h"

#if __cplusplus >= 201103L
#define typeof decltype
#endif

namespace Realm {

  class IndexSpaceImpl;
  class ProcessorGroup;
  class MemoryImpl;
  class ProcessorImpl;
  class RegionInstanceImpl;
  class Module;

#ifdef USE_GASNET 
    class HandlerThread : public PreemptableThread {
    public:
      HandlerThread(IncomingMessageManager *m) : manager(m) { }
      virtual ~HandlerThread(void) { }
    public:
      virtual Processor get_processor(void) const 
        { assert(false); return Processor::NO_PROC; }
    public:
      virtual void thread_main(void);
      virtual void sleep_on_event(Event wait_for);
    public:
      void join(void);
    private:
      IncomingMessage *current_msg, *next_msg;
      IncomingMessageManager *const manager;
    };
#endif

    template <typename _ET, size_t _INNER_BITS, size_t _LEAF_BITS>
    class DynamicTableAllocator {
    public:
      typedef _ET ET;
      static const size_t INNER_BITS = _INNER_BITS;
      static const size_t LEAF_BITS = _LEAF_BITS;

      typedef GASNetHSL LT;
      typedef int IT;
      typedef DynamicTableNode<DynamicTableNodeBase<LT, IT> *, 1 << INNER_BITS, LT, IT> INNER_TYPE;
      typedef DynamicTableNode<ET, 1 << LEAF_BITS, LT, IT> LEAF_TYPE;
      typedef DynamicTableFreeList<DynamicTableAllocator<ET, _INNER_BITS, _LEAF_BITS> > FreeList;
      
      static LEAF_TYPE *new_leaf_node(IT first_index, IT last_index, 
				      int owner, FreeList *free_list)
      {
	LEAF_TYPE *leaf = new LEAF_TYPE(0, first_index, last_index);
	IT last_ofs = (((IT)1) << LEAF_BITS) - 1;
	for(IT i = 0; i <= last_ofs; i++)
	  leaf->elems[i].init(ID(ET::ID_TYPE, owner, first_index + i).convert<typeof(leaf->elems[0].me)>(), owner);

	if(free_list) {
	  // stitch all the new elements into the free list
	  free_list->lock.lock();

	  for(IT i = 0; i <= last_ofs; i++)
	    leaf->elems[i].next_free = ((i < last_ofs) ? 
					  &(leaf->elems[i+1]) :
					  free_list->first_free);

	  free_list->first_free = &(leaf->elems[first_index ? 0 : 1]);

	  free_list->lock.unlock();
	}

	return leaf;
      }
    };

    typedef DynamicTableAllocator<GenEventImpl, 10, 8> EventTableAllocator;
    typedef DynamicTableAllocator<BarrierImpl, 10, 4> BarrierTableAllocator;
    typedef DynamicTableAllocator<ReservationImpl, 10, 8> ReservationTableAllocator;
    typedef DynamicTableAllocator<IndexSpaceImpl, 10, 4> IndexSpaceTableAllocator;
    typedef DynamicTableAllocator<ProcessorGroup, 10, 4> ProcessorGroupTableAllocator;

    // for each of the ID-based runtime objects, we're going to have an
    //  implementation class and a table to look them up in
    struct Node {
      Node(void);

      // not currently resizable
      std::vector<MemoryImpl *> memories;
      std::vector<ProcessorImpl *> processors;

      DynamicTable<EventTableAllocator> events;
      DynamicTable<BarrierTableAllocator> barriers;
      DynamicTable<ReservationTableAllocator> reservations;
      DynamicTable<IndexSpaceTableAllocator> index_spaces;
      DynamicTable<ProcessorGroupTableAllocator> proc_groups;
    };

    class RuntimeImpl {
    public:
      RuntimeImpl(void);
      ~RuntimeImpl(void);

      bool init(int *argc, char ***argv);

      bool register_task(Processor::TaskFuncID taskid, Processor::TaskFuncPtr taskptr);
      bool register_reduction(ReductionOpID redop_id, const ReductionOpUntyped *redop);

      void run(Processor::TaskFuncID task_id = 0, 
	       Runtime::RunStyle style = Runtime::ONE_TASK_ONLY,
	       const void *args = 0, size_t arglen = 0, bool background = false);

      // requests a shutdown of the runtime
      void shutdown(bool local_request);

      void wait_for_shutdown(void);

      // three event-related impl calls - get_event_impl() will give you either
      //  a normal event or a barrier, but you won't be able to do specific things
      //  (e.g. trigger a GenEventImpl or adjust a BarrierImpl)
      EventImpl *get_event_impl(Event e);
      GenEventImpl *get_genevent_impl(Event e);
      BarrierImpl *get_barrier_impl(Event e);

      ReservationImpl *get_lock_impl(ID id);
      MemoryImpl *get_memory_impl(ID id);
      ProcessorImpl *get_processor_impl(ID id);
      ProcessorGroup *get_procgroup_impl(ID id);
      IndexSpaceImpl *get_index_space_impl(ID id);
      RegionInstanceImpl *get_instance_impl(ID id);
#ifdef DEADLOCK_TRACE
      void add_thread(const pthread_t *thread);
#endif

    protected:
    public:
      MachineImpl *machine;

      Processor::TaskIDTable task_table;
      std::map<ReductionOpID, const ReductionOpUntyped *> reduce_op_table;

#ifdef NODE_LOGGING
      static const char *prefix;
#endif

      std::vector<Module *> modules;
      Node *nodes;
      MemoryImpl *global_memory;
      EventTableAllocator::FreeList *local_event_free_list;
      BarrierTableAllocator::FreeList *local_barrier_free_list;
      ReservationTableAllocator::FreeList *local_reservation_free_list;
      IndexSpaceTableAllocator::FreeList *local_index_space_free_list;
      ProcessorGroupTableAllocator::FreeList *local_proc_group_free_list;

      pthread_t *background_pthread;
#ifdef DEADLOCK_TRACE
      unsigned next_thread;
      unsigned signaled_threads;
      pthread_t all_threads[MAX_NUM_THREADS];
      unsigned thread_counts[MAX_NUM_THREADS];
#endif
    };

    extern RuntimeImpl *runtime_singleton;
    inline RuntimeImpl *get_runtime(void) { return runtime_singleton; }

    // active messages

    struct RuntimeShutdownMessage {
      struct RequestArgs {
	int initiating_node;
	int dummy; // needed to get sizeof() >= 8
      };

      static void handle_request(RequestArgs args);

      typedef ActiveMessageShortNoReply<MACHINE_SHUTDOWN_MSGID,
				        RequestArgs,
				        handle_request> Message;

      static void send_request(gasnet_node_t target);
    };
      
}; // namespace Realm

#endif // ifndef REALM_RUNTIME_IMPL_H
