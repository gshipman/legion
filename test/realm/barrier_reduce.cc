#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>

#include <time.h>

#include "realm/realm.h"

using namespace Realm;

// Task IDs, some IDs are reserved so start at first available number
enum {
  TOP_LEVEL_TASK = Processor::TASK_ID_FIRST_AVAILABLE+0,
  CHILD_TASK     = Processor::TASK_ID_FIRST_AVAILABLE+1,
};

enum { REDOP_ADD = 1 };

class ReductionOpIntAdd {
public:
  typedef int LHS;
  typedef int RHS;

  template <bool EXCL>
  static void apply(LHS& lhs, RHS rhs) { lhs += rhs; }

  // both of these are optional
  static const RHS identity;

  template <bool EXCL>
  static void fold(RHS& rhs1, RHS rhs2) { rhs1 += rhs2; }
};

const ReductionOpIntAdd::RHS ReductionOpIntAdd::identity = 0;

struct ChildTaskArgs {
  size_t num_iters;
  size_t index;
  Barrier b;
};

static const int BARRIER_INITIAL_VALUE = 42;

static int errors = 0;

void child_task(const void *args, size_t arglen, Processor p)
{
  assert(arglen == sizeof(ChildTaskArgs));
  const ChildTaskArgs& child_args = *(const ChildTaskArgs *)args;

  printf("starting child task %zd on processor " IDFMT "\n", child_args.index, p.id);
  Barrier b = child_args.b;  // so we can advance it
  for(size_t i = 0; i < child_args.num_iters; i++) {
    int reduce_val = (i+1)*(child_args.index+1);
    b.arrive(1, Event::NO_EVENT, &reduce_val, sizeof(reduce_val));

    // is it our turn to wait on the barrier?
    if(i == child_args.index) {
      int result;
      bool ready = b.get_result(&result, sizeof(result));
      if(!ready) {
	// wait on barrier to be ready and then ask for result again
	b.wait();
	bool ready2 = b.get_result(&result, sizeof(result));
	assert(ready2);
      }
      int exp_result = BARRIER_INITIAL_VALUE + (i+1)*child_args.num_iters*(child_args.num_iters + 1) / 2;
      if(result == exp_result)
	printf("child %zd: iter %zd = %d (%d) OK\n", child_args.index, i, result, ready);
      else {
	printf("child %zd: iter %zd = %d (%d) ERROR (expected %d)\n", child_args.index, i, result, ready, exp_result);
	errors++;
      }
    }

#ifdef SHARED_LOWLEVEL
    // shared LLR's barriers assume all arrivals are stratified (i.e. no arrivals for phase i+1
    //  before all arrivals from phase i are seen) - this is broken, but work around it here
    b.wait();
#endif

    b = b.advance_barrier();
  }

  printf("ending child task %zd on processor " IDFMT "\n", child_args.index, p.id);
}

void top_level_task(const void *args, size_t arglen, Processor p)
{
  printf("top level task - getting machine and list of CPUs\n");

  Machine machine = Machine::get_machine();
  std::vector<Processor> all_cpus;
  {
    std::set<Processor> all_processors;
    machine.get_all_processors(all_processors);
    for(std::set<Processor>::const_iterator it = all_processors.begin();
	it != all_processors.end();
	it++)
      if((*it).kind() == Processor::LOC_PROC)
	all_cpus.push_back(*it);
  }

  printf("top level task - creating barrier\n");

  Barrier b = Barrier::create_barrier(all_cpus.size(), REDOP_ADD,
				      &BARRIER_INITIAL_VALUE, sizeof(BARRIER_INITIAL_VALUE));

  std::set<Event> task_events;
  for(size_t i = 0; i < all_cpus.size(); i++) {
    ChildTaskArgs args;
    args.num_iters = all_cpus.size();
    args.index = i;
    args.b = b;

    Event e = all_cpus[i].spawn(CHILD_TASK, &args, sizeof(args));
    task_events.insert(e);
  }
  printf("%zd tasks launched\n", task_events.size());;

  // now wait on each generation of the barrier and report the result
  for(size_t i = 0; i < all_cpus.size(); i++) {
    int result;
    bool ready = b.get_result(&result, sizeof(result));
    if(!ready) {
      // wait on barrier to be ready and then ask for result again
      b.wait();
      bool ready2 = b.get_result(&result, sizeof(result));
      assert(ready2);
    }
    int exp_result = BARRIER_INITIAL_VALUE + (i+1)*all_cpus.size()*(all_cpus.size() + 1) / 2;
    if(result == exp_result)
      printf("parent: iter %zd = %d (%d) OK\n", i, result, ready);
    else {
      printf("parent: iter %zd = %d (%d) ERROR (expected %d)\n", i, result, ready, exp_result);
      errors++;
    }

    b = b.advance_barrier();
  }

  // wait on all child tasks to finish before destroying barrier
  Event merged = Event::merge_events(task_events);
  printf("merged event ID is " IDFMT "/%d - waiting on it...\n",
	 merged.id, merged.gen);
  merged.wait();

  b.destroy_barrier();

  if(errors > 0) {
    printf("Exiting with errors.\n");
    exit(1);
  }

  printf("done!\n");

  Runtime::get_runtime().shutdown();
}

int main(int argc, char **argv)
{
  Runtime rt;

  rt.init(&argc, &argv);

  rt.register_task(TOP_LEVEL_TASK, top_level_task);
  rt.register_task(CHILD_TASK, child_task);

  rt.register_reduction(REDOP_ADD, 
			ReductionOpUntyped::create_reduction_op<ReductionOpIntAdd>());

  // Start the machine running
  // Control never returns from this call
  // Note we only run the top level task on one processor
  // You can also run the top level task on all processors or one processor per node
  rt.run(TOP_LEVEL_TASK, Runtime::ONE_TASK_ONLY);

  return 0;
}