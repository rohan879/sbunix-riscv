# SBUnix Scheduler Enhancement Proposal

## Current State Analysis

### Existing Infrastructure ✅

The sbunix-skeleton already has a basic scheduling foundation:

- **Simple round-robin scheduler** in [proc.c](kernel/proc.c)
- **Working context switch** via [swtch.S](kernel/swtch.S)
- **Timer interrupts** configured and running in [trap.c](kernel/trap.c)
- **Process table** with 64 process slots
- **Basic process states**: `UNUSED`, `SLEEPING`, `RUNNABLE`, `RUNNING`, `ZOMBIE`

### Current Scheduler Code

```c
// Current scheduler() in proc.c - Simple round-robin
void scheduler(void) {
  struct proc *p;
  for(;;){
    for(p = proc; p < &proc[64]; p++) {
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        current_proc = p;
        swtch(&scheduler_context, &p->context);
        current_proc = 0;
      }
    }
  }
}
```

## Proposed Enhancements

### Phase 1: Preemptive Scheduling (High Priority)

#### Goal
Add timer-based preemption to enable fair time sharing.

#### Implementation Location
- **Primary**: [trap.c](kernel/trap.c) - modify `handle_timer_interrupt()`
- **Secondary**: [proc.c](kernel/proc.c) - enhance process structure

#### Changes Required

**1. Add time slice tracking to process structure**

```c
// Addition to struct proc in proc.h
struct proc {
  enum procstate state;
  pid_t pid;
  struct proc *parent;
  
  // NEW SCHEDULING FIELDS
  int time_slice;           // Remaining time quantum (in ticks)
  int priority;             // Process priority (0-139, lower = higher priority)
  int nice;                 // Nice value (-20 to +19)
  unsigned long total_runtime;  // Total CPU time consumed
  
  // Existing fields...
  void *kstack;
  uint64_t sz;
  pagetable_t pagetable;
  struct trapframe *trapframe;
  struct context context;
  char name[16];
};
```

**2. Modify timer interrupt handler for preemption**

```c
// Enhanced handle_timer_interrupt() in trap.c
static void handle_timer_interrupt(void) {
    ticks++;
    display_time();
    
    // SCHEDULER TICK - Check for preemption
    if (current_proc && current_proc->state == RUNNING) {
        current_proc->total_runtime++;
        
        // Decrement time slice
        if (--current_proc->time_slice <= 0) {
            // Time slice expired - trigger preemption
            current_proc->state = RUNNABLE;
            yield();  // Force context switch
        }
    }
    
    timer_set_next();
}
```

**3. Update process allocation to set initial time slice**

```c
// Enhanced allocproc() in proc.c
struct proc* allocproc(void) {
  struct proc *p;
  
  // ... existing allocation code ...
  
  // Initialize scheduling fields
  p->time_slice = DEFAULT_TIME_SLICE;  // e.g., 10 ticks
  p->priority = 120;  // Default priority (nice 0)
  p->nice = 0;
  p->total_runtime = 0;
  
  return p;
}
```

### Phase 2: Time-Quantum Round Robin (Medium Priority)

#### Goal
Implement proper round-robin scheduling with configurable time quanta.

#### Enhancements

**1. Add scheduler configuration**

```c
// Scheduler configuration constants
#define DEFAULT_TIME_SLICE  10    // 10 timer ticks per process
#define MIN_TIME_SLICE      1     // Minimum time slice
#define MAX_TIME_SLICE      100   // Maximum time slice

// Scheduler statistics
struct sched_stats {
    unsigned long context_switches;
    unsigned long total_processes;
    unsigned long timer_interrupts;
    unsigned long preemptions;
};

extern struct sched_stats sched_stats;
```

**2. Enhanced scheduler with time slice reset**

```c
// Enhanced scheduler() in proc.c
void scheduler(void) {
  struct proc *p;
  
  for(;;){
    for(p = proc; p < &proc[64]; p++) {
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        current_proc = p;
        
        // Reset time slice for new run
        p->time_slice = calculate_time_slice(p);
        
        // Update statistics
        sched_stats.context_switches++;
        
        swtch(&scheduler_context, &p->context);
        
        // Process gave up CPU
        current_proc = 0;
      }
    }
  }
}

// Calculate time slice based on priority/nice value
static int calculate_time_slice(struct proc *p) {
    // Higher priority (lower number) gets more time
    int base_slice = DEFAULT_TIME_SLICE;
    
    // Adjust based on nice value
    if (p->nice < 0) {
        base_slice += (-p->nice) * 2;  // High priority gets bonus
    } else {
        base_slice -= p->nice / 2;     // Low priority gets penalty
    }
    
    // Clamp to valid range
    if (base_slice < MIN_TIME_SLICE) base_slice = MIN_TIME_SLICE;
    if (base_slice > MAX_TIME_SLICE) base_slice = MAX_TIME_SLICE;
    
    return base_slice;
}
```

### Phase 3: Priority-Based Scheduling (Lower Priority)

#### Goal
Implement priority queues and better process prioritization.

#### Implementation Approach

**1. Add runqueue data structure**

```c
// Priority-based runqueue structure
#define NUM_PRIORITY_LEVELS 140  // 0-99 RT, 100-139 normal

struct runqueue {
    struct proc *head[NUM_PRIORITY_LEVELS];
    struct proc *tail[NUM_PRIORITY_LEVELS];
    int count[NUM_PRIORITY_LEVELS];
    unsigned long priority_bitmap;  // Bitmap of non-empty queues
};

static struct runqueue rq;
```

**2. Priority-aware scheduling functions**

```c
// Enqueue process in priority runqueue
void enqueue_task(struct proc *p) {
    int prio = p->priority;
    
    if (rq.tail[prio]) {
        rq.tail[prio]->next = p;
    } else {
        rq.head[prio] = p;
        // Set bit in priority bitmap
        rq.priority_bitmap |= (1UL << prio);
    }
    
    rq.tail[prio] = p;
    p->next = NULL;
    rq.count[prio]++;
}

// Dequeue highest priority process
struct proc* dequeue_task(void) {
    if (rq.priority_bitmap == 0) return NULL;
    
    // Find highest priority (lowest number)
    int prio = __builtin_clzl(rq.priority_bitmap);
    struct proc *p = rq.head[prio];
    
    if (p) {
        rq.head[prio] = p->next;
        if (!rq.head[prio]) {
            rq.tail[prio] = NULL;
            rq.priority_bitmap &= ~(1UL << prio);
        }
        rq.count[prio]--;
        p->next = NULL;
    }
    
    return p;
}

// Priority-based scheduler
void scheduler_priority(void) {
    struct proc *p;
    
    for(;;) {
        p = dequeue_task();
        if (p && p->state == RUNNABLE) {
            p->state = RUNNING;
            current_proc = p;
            p->time_slice = calculate_time_slice(p);
            
            swtch(&scheduler_context, &p->context);
            
            // Update priority after run (aging)
            if (p->state == RUNNABLE) {
                update_priority(p);
                enqueue_task(p);
            }
            
            current_proc = 0;
        }
    }
}
```

### Phase 4: Advanced Features (Future Enhancement)

#### Completely Fair Scheduler (CFS-like)

```c
// Virtual runtime tracking for fairness
struct proc {
    // ... existing fields ...
    
    unsigned long vruntime;      // Virtual runtime
    unsigned long min_vruntime;  // Minimum vruntime seen
    struct rb_node run_node;     // Red-black tree node
};

// Red-black tree for CFS
struct cfs_rq {
    struct rb_root_cached tasks_timeline;
    struct proc *curr;
    unsigned long min_vruntime;
    int nr_running;
};
```

#### Real-Time Scheduling Classes

```c
// Scheduling classes
enum sched_class_type {
    SCHED_CLASS_RT,      // Real-time FIFO/RR
    SCHED_CLASS_NORMAL,  // CFS/Priority
    SCHED_CLASS_IDLE     // Idle processes
};

struct sched_class {
    enum sched_class_type type;
    void (*enqueue_task)(struct proc *p);
    void (*dequeue_task)(struct proc *p);
    struct proc* (*pick_next_task)(void);
    void (*task_tick)(struct proc *p);
    void (*task_new)(struct proc *p);
};
```

## Implementation Timeline

### Week 1: Basic Preemption
- [ ] Add time_slice field to `struct proc`
- [ ] Modify timer interrupt handler
- [ ] Test preemptive multitasking with multiple processes

### Week 2: Round Robin Enhancement
- [ ] Implement time slice calculation
- [ ] Add scheduler statistics
- [ ] Create process priority management functions

### Week 3: Priority Scheduling
- [ ] Implement runqueue data structure
- [ ] Add priority-based enqueue/dequeue
- [ ] Test priority inheritance and aging

### Week 4: Advanced Features
- [ ] Begin CFS implementation OR
- [ ] Add real-time scheduling classes
- [ ] Performance tuning and optimization

## Testing Strategy

### Test Scenarios

1. **Single Process**: Verify basic operation
2. **CPU-bound Processes**: Test time-slice preemption
3. **I/O-bound Processes**: Verify sleep/wakeup behavior
4. **Mixed Workload**: Test priority handling
5. **Stress Test**: Many processes competing for CPU

### Debugging Support

```c
// Scheduler debugging functions
void sched_debug_print_runqueue(void);
void sched_debug_print_process_stats(struct proc *p);
void sched_debug_toggle_verbose(void);

// Add to proc.c
#ifdef SCHED_DEBUG
#define sched_debug(fmt, ...) printk("[SCHED] " fmt, ##__VA_ARGS__)
#else
#define sched_debug(fmt, ...) 
#endif
```

## Performance Considerations

### Optimization Opportunities

1. **Fast Path**: Optimize common scheduler operations
2. **Cache Locality**: Keep frequently accessed data together
3. **Lock-Free Operations**: Minimize synchronization overhead
4. **Batch Operations**: Group related scheduler tasks

### Metrics to Track

- Context switch overhead
- Scheduler latency
- Throughput (processes/second)
- Fairness index
- Response time distribution

## Integration with Existing Code

### Required File Modifications

1. **[proc.h](kernel/include/proc.h)**: Add scheduling fields to `struct proc`
2. **[proc.c](kernel/proc.c)**: Enhance scheduler and process management
3. **[trap.c](kernel/trap.c)**: Add preemption to timer interrupt
4. **[kernel.c](kernel/kernel.c)**: Initialize scheduler statistics

### New Files (Optional)

- **[sched.c](kernel/sched.c)**: Advanced scheduling algorithms
- **[sched.h](kernel/include/sched.h)**: Scheduler interface definitions

## Backward Compatibility

All enhancements will maintain compatibility with existing process creation and management code. The current `yield()` function and process states remain unchanged.

## Conclusion

This enhancement proposal provides a clear path from the current simple round-robin scheduler to a sophisticated multi-class scheduler. The phased approach allows for incremental development and testing, ensuring system stability throughout the enhancement process.

The foundation provided by sbunix-skeleton's existing timer interrupts, context switching, and process management makes it an excellent platform for implementing advanced scheduling algorithms.