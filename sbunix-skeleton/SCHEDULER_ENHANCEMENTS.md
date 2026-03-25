# SBUnix Scheduler Enhancement Proposal
**Updated:** March 25, 2026  
**Status:** ✅ Priority 1 COMPLETE → 🎯 Priority 2 CFS Implementation Ready  
**Build Status:** ✅ Current dev-w builds successfully

---

## Current State Analysis (Updated Assessment)

### ✅ Implemented Foundation (Priority 1 Complete)

The sbunix kernel now has **comprehensive scheduling infrastructure**:

- **✅ Preemptive round-robin scheduler** with timer-driven yield
- **✅ Advanced context switching** (14 RISC-V registers via swtch.S)  
- **✅ Timer interrupt preemption** (~100μs quantum, configurable)
- **✅ Process table** with 128 process slots (NPROC=128)
- **✅ Complete process lifecycle**: fork, exec, exit, wait, zombie reaping
- **✅ Signal delivery** (SIGALRM, SIGINT, SIGCHLD, SIGSEGV) 
- **✅ WFI idle optimization** for power efficiency
- **✅ COW fork** with page refcounting and demand paging
- **✅ VFS integration** with procfs process information

### 🔄 Current Scheduler Implementation

```c
// Enhanced scheduler() in proc.c - Preemptive round-robin  
void scheduler(void) {
  struct proc *p;
  for(;;){
    w_sstatus(r_sstatus() | SSTATUS_SIE);  // Enable interrupts
    
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {   // O(128) scan
      if(p->state == RUNNABLE) {
        found = 1;
        p->state = RUNNING;
        current_proc = p;
        
        // Interrupt-safe context switch
        w_sstatus(r_sstatus() & ~SSTATUS_SIE);
        asm volatile("csrw sscratch, %0" : : "r"((uint64_t)p->kstack + PAGE_SIZE));
        
        if (p->pagetable != kernel_pagetable) {
            w_satp(MAKE_SATP(p->pagetable));
            sfence_vma();
        }
        
        swtch(&scheduler_context, &p->context);
        
        // Return to kernel space
        w_satp(MAKE_SATP(kernel_pagetable)); 
        sfence_vma();
        current_proc = 0;
      }
    }
    
    if (!found)
      asm volatile("wfi");  // Power-efficient idle
  }
}
```

```

### 🎯 Timer Interrupt Preemption (Implemented)

```c
// Current handle_timer_interrupt() in trap.c - Preemptive scheduling active
static void handle_timer_interrupt(void) {
    ticks++;
    
    // 1. Wake timed-sleep processes 
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p->waiting_for_uart == 2 &&
            r_time() >= p->sleep_until) {
            p->waiting_for_uart = 0;
            p->state = RUNNABLE;
        }
    }
    
    // 2. SIGALRM delivery for alarm() syscall
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->alarm_ticks > 0 && ticks >= p->alarm_ticks) {
            p->alarm_ticks = 0;
            p->sigpending |= signal_bit(SIGALRM);
            if (p->state == SLEEPING) p->state = RUNNABLE;
        }
    }
    
    // 3. PREEMPTION: Timer tick forces round-robin yield
    if (current_proc && current_proc->state == RUNNING) {
        current_proc->state = RUNNABLE;  // Yield CPU
        swtch(&current_proc->context, &scheduler_context);
    }
    
    timer_set_next();
}
```

---

## 🚀 Priority 2: CFS (Completely Fair Scheduler) Implementation

### Current Limitations Requiring CFS

Based on comprehensive analysis, **Priority 1 scheduler has fundamental scalability issues**:

| Issue | Current State | CFS Solution |
|-------|---------------|--------------|
| **Scheduling Cost** | O(128) scan per tick | O(log n) RB-tree lookup |
| **Fairness** | Fixed quantum for all | Virtual runtime weighting |
| **Priority Support** | nice() no-op | Full priority-to-weight mapping |
| **Load Tracking** | None | Load average and load balancing |
| **Time Accounting** | Basic ticks only | User/kernel time separation |
| **SMP Readiness** | Global shared state | Per-CPU runqueues |

### CFS Requirements Analysis

#### 1. Virtual Runtime & Time Accounting ❌ **MISSING**
```c
// Required additions to struct proc:
uint64_t se_vruntime;         // Virtual runtime (weighted execution time)
uint64_t exec_start;          // When current exec interval started  
uint64_t sum_exec_runtime;    // Total real time executed
uint64_t utime, stime;        // User mode / kernel mode time
unsigned long se_weight;      // Load weight from nice value
```

#### 2. Priority-to-Weight Mapping ❌ **MISSING**
```c
// Linux CFS weight table (from nice -20 to +19)
static const unsigned long prio_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,  
    /* -10 */ 9548,  7620,  6100,  4904,  3906,
    /*  -5 */ 3121,  2501,  1991,  1586,  1277,
    /*   0 */ 1024,  820,   655,   526,   423,    // ← Nice 0 baseline
    /*   5 */ 335,   272,   215,   172,   137,
    /*  10 */ 110,   87,    70,    56,    45,
    /*  15 */ 36,    29,    23,    18,    15,
};

// Virtual time calculation:
// delta_vruntime = (delta_exec * NICE_0_LOAD) / task_weight
// where NICE_0_LOAD = 1024, task_weight = prio_to_weight[nice+20]
```

#### 3. Red-Black Tree Runqueue ❌ **MISSING**
```c
// CFS runqueue structure
struct cfs_rq {
    struct rb_root *tasks_timeline;  // RB-tree root
    struct rb_node *rb_leftmost;     // Cached leftmost (min vruntime)
    unsigned long nr_running;        // Number of runnable tasks
    uint64_t min_vruntime;           // Monotonic vruntime floor
    unsigned long load_avg;          // Aggregate load weight
};
```

#### 4. Load Average & Balancing ❌ **MISSING** 
```c
// Per-CPU load tracking
struct cpu_load {
    unsigned long load_avg_contrib;  // This CPU's load contribution
    uint64_t last_update;           // Last load calculation time
    unsigned long nr_running;       // Current runnable tasks
};

// System load average (for /proc/loadavg)
unsigned long global_load_avg[3];  // 1min, 5min, 15min averages
```

---

## 📋 CFS Implementation Roadmap

### **Phase 2A: Time Accounting Foundation** (2-3 weeks)
**Goal:** Add virtual runtime tracking and priority support

#### Core Changes Required:
1. **Expand struct proc** with CFS fields
2. **Implement prio_to_weight mapping**  
3. **Add vruntime calculation** in timer interrupt
4. **Implement nice/setpriority syscalls**

#### Key Files to Modify:
- `kernel/include/proc.h` - Add CFS fields to struct proc
- `kernel/proc.c` - Update allocproc() for initialization
- `kernel/trap.c` - Enhance timer interrupt with vruntime tracking  
- `kernel/trap.c` - Add setpriority/getpriority/nice syscall handlers
- `libc/include/unistd.h` - Expose nice() prototype
- `libc/unistd.c` - Fix nice() implementation

#### Implementation Details:
```c
// Enhanced struct proc (kernel/include/proc.h)
struct proc {
    // ... existing fields ...
    
    // CFS Scheduling Entity
    uint64_t se_vruntime;           // Virtual runtime
    uint64_t exec_start;            // Execution interval start time
    uint64_t sum_exec_runtime;      // Total runtime
    unsigned long se_weight;        // Priority weight
    
    // Priority Management  
    int nice;                       // Nice value (-20 to +19)
    int prio;                       // Dynamic priority (100-139)
    int static_prio;               // Static priority
    unsigned long policy;           // SCHED_OTHER, SCHED_FIFO, etc.
    
    // Time Accounting
    uint64_t utime;                 // User mode time
    uint64_t stime;                 // System mode time
    uint64_t nvcsw, nivcsw;         // Voluntary/involuntary context switches
    
    // RB-tree node (for Phase 2B)
    struct rb_node run_node;        // Position in CFS runqueue
};
```

### **Phase 2B: RB-Tree Runqueue** (3-4 weeks)
**Goal:** Replace O(n) scheduler with O(log n) tree-based runqueue

#### Core Components:
1. **Red-black tree implementation** for process ordering by vruntime
2. **Enqueue/dequeue operations** maintaining tree balance
3. **Pick-next-task** via leftmost node (minimum vruntime)
4. **Tree rebalancing** on priority changes

#### Scheduler Transformation:
```c
// NEW: CFS scheduler with RB-tree (kernel/proc.c)
void scheduler(void) {
    struct cpu *c = mycpu();
    
    for(;;) {
        intr_on();  // Enable interrupts while idle
        
        // Pick left-most task (minimum vruntime) - O(1)
        struct sched_entity *se = pick_next_entity(&c->cfs);
        if (!se) {
            asm volatile("wfi");
            continue;
        }
        
        struct proc *p = se_to_proc(se);
        run_task(p);  // Execute with timer accounting
    }
}

// CFS core: pick task with smallest vruntime  
struct sched_entity* pick_next_entity(struct cfs_rq *cfs_rq) {
    struct rb_node *leftmost = cfs_rq->rb_leftmost;
    if (!leftmost) return NULL;
    return rb_entry(leftmost, struct sched_entity, run_node);
}
```

### **Phase 2C: Per-CPU Infrastructure** (2-3 weeks) 
**Goal:** Enable multicore scaling with per-CPU runqueues

#### SMP Architecture:
1. **Per-CPU data structures** (struct cpu array)
2. **mycpu()/myproc()** accessor functions using RISC-V tp register  
3. **Convert global variables** to per-CPU access
4. **Interrupt-safe CPU identification** with hartid

#### Multicore Foundation:
```c
// Per-CPU structure (kernel/include/proc.h)
#define NCPU 8

struct cpu {
    struct proc *proc;          // Currently running process
    struct context context;     // Scheduler context  
    struct cfs_rq cfs;          // This CPU's CFS runqueue
    int id;                     // Hardware thread ID (hartid)
    int noff;                   // Interrupt disable nesting level
    int intena;                 // Enable flags for nested interrupts
};

extern struct cpu cpus[NCPU];

// RISC-V hart identification
static inline int get_hartid(void) {
    int id;
    asm volatile("mv %0, tp" : "=r"(id));
    return id;
}

struct cpu* mycpu(void) {
    int id = get_hartid();
    return &cpus[id];
}

struct proc* myproc(void) {
    return mycpu()->proc;
}
```

### **Phase 2D: Load Balancing & Work Stealing** (3-4 weeks)
**Goal:** Optimize workload distribution across multiple CPUs

#### Load Balancing Strategy:
1. **Load average calculation** via EWMA (exponentially weighted moving average)
2. **Work stealing** when local runqueue is empty
3. **Migration decisions** based on load imbalance thresholds  
4. **Idle CPU wakeup** for newly created tasks

#### Implementation Components:
```c
// Load balancing (kernel/sched_fair.c - new file)
void load_balance(struct cpu *this_cpu) {
    if (this_cpu->cfs.nr_running > 0) return;  // Local work available
    
    // Find most loaded CPU
    struct cpu *busiest = find_busiest_cpu();
    if (!busiest) return;
    
    // Attempt task migration  
    struct sched_entity *se = steal_task(busiest);
    if (se) {
        enqueue_entity(&this_cpu->cfs, se);
        wakeup_cpu(this_cpu->id);
    }
}

// Periodic load average update
void update_load_averages(void) {
    for (int i = 0; i < NCPU; i++) {
        struct cpu *c = &cpus[i];
        
        // EWMA: new_avg = 0.9 * old_avg + 0.1 * current_load
        c->load_avg = (9 * c->load_avg + c->cfs.nr_running) / 10;
    }
    
    // System-wide load average for /proc/loadavg
    calculate_global_load_avg();
}
```

---

## ⚙️ Testing & Validation Framework

### Performance Benchmarks
1. **Scheduling Latency:** Measure context switch overhead (target: <200 cycles)
2. **Fairness Validation:** Multi-process CPU sharing equality (±5% variance)
3. **Scalability Testing:** Performance vs. process count (up to 128 processes)  
4. **SMP Scaling:** Efficiency with 2, 4, 8 CPU cores (target: >80% at 4 cores)

### Test Programs  
```c
// CPU fairness test (bin/cfstestStress test: multiple CPU-bound processes
int main() {
    int pid;
    
    for (int i = 0; i < 8; i++) {
        if ((pid = fork()) == 0) {
            // Child: CPU-intensive loop
            for (long j = 0; j < 100000000; j++) {
                asm volatile("nop");  // Burn CPU
            }
            printf("Child %d completed\n", i);
            exit(0);
        }
    }
    
    // Parent: wait and measure fairness
    while (wait(NULL) > 0) { /* reap children */ }
}
```

### Integration Tests
1. **Load average reporting** via `/proc/loadavg`  
2. **Priority inheritance** with `nice` command
3. **Signal delivery** during scheduler transitions
4. **Memory pressure** interaction with swap/paging

---

## 🎯 Success Metrics

| Metric | Current | CFS Target |
|--------|---------|------------|
| **Scheduling Cost** | O(128) per tick | O(log n) ≤ O(7) |
| **Context Switch** | ~200 cycles | ~200 cycles (maintain) |
| **Fairness Error** | High variance | ≤5% CPU time variance |
| **Load Reporting** | None | Real-time load averages |
| **SMP Scaling** | N/A (single CPU) | 80% efficiency @4 cores |
| **Nice Support** | No-op | ±25% CPU share per nice level |

**Completion Timeline:** 10-12 weeks for full CFS implementation
**Priority:** High - foundational for all advanced OS features

---

*Updated March 25, 2026 - Ready for Priority 2 CFS implementation*

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