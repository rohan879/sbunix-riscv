# Chapter 2: Deciphering the Process Scheduler

## Overview

Process scheduling is one of the most crucial executive jobs of any operating system, Linux being no different. The heuristics and efficiency in scheduling processes is what make any operating system tick and also give it an identity, such as a general-purpose operating system, server, or a real-time system. 

This chapter covers:
- Linux scheduler design
- Scheduling classes
- Scheduling policies and priorities 
- Completely Fair Scheduler
- Real-Time Scheduler
- Deadline Scheduler
- Group scheduling
- Preemption

## Process Schedulers

The effectiveness of any operating system is proportional to its ability to fairly schedule all contending processes. The process scheduler is the core component of the kernel, which computes and decides when and for how long a process gets CPU time. Ideally, processes require a timeslice of the CPU to run, so schedulers essentially need to allocate slices of processor time fairly among processes. 

A scheduler typically has to:
- Avoid process starvation
- Manage priority scheduling
- Maximize throughput of all processes
- Ensure low turnaround time
- Ensure even resource usage
- Avoid CPU hogging
- Consider process' behavioral patterns for prioritization
- Elegantly subsidize under heavy load
- Handle scheduling on multiple cores efficiently

## Linux Process Scheduler Design

Linux, which was primarily developed for desktop systems, has unassumingly evolved into a multidimensional operating system with its usage spread across embedded devices, mainframes, and supercomputers to room-sized servers. It has also seamlessly accommodated the ever-evolving diverse computing platforms such as SMP, virtualization, and real-time systems. 

The diversity of these platforms is brought forth by the kind of processes that run on these systems. For instance, a highly interactive desktop system may run processes that are I/O bound, and a real-time system thrives on deterministic processes.

### Two-Layered Architecture

The intrinsic design of Linux's process scheduler elegantly and deftly handles this challenge by adopting a simple **two-layered model**:

1. **Generic Scheduler** (First layer): Defines abstract operations that serve as entry functions for the scheduler
2. **Scheduling Classes** (Second layer): Implement the actual scheduling operations, where each class is dedicated to handling the scheduling heuristics of a particular kind of process

This model enables the generic scheduler to remain abstracted from the implementation details of every scheduler class. For instance:
- Normal processes (I/O bound) can be handled by one class
- Processes that require deterministic execution (real-time) can be handled by another class
- This architecture also enables adding a new scheduling class seamlessly

### The sched_class Structure

The generic scheduler defines abstract interfaces through a structure called `sched_class`:

```c
struct sched_class {
    const struct sched_class *next;
    void (*enqueue_task) (struct rq *rq, struct task_struct* p, int flags);
    void (*dequeue_task) (struct rq* rq, struct task_struct *p, int flags);
    void (*yield_task) (struct rq *rq);
    bool (*yield_to_task) (struct rq *rq, struct task_struct* p, bool preempt);
    void (*check_preempt_curr) (struct rq* rq, struct task_struct *p, int flags);
    
    /*
     * It is the responsibility of the pick_next_task() method that will
     * return the next task to call put_prev_task() on the @prev task or
     * something equivalent.
     * May return RETRY_TASK when it finds a higher prio class has runnable
     * tasks.
     */
    struct task_struct* (*pick_next_task) (struct rq* rq, struct task_struct *prev, struct rq_flags* rf);
    void (*put_prev_task) (struct rq* rq, struct task_struct *p);
    
#ifdef CONFIG_SMP
    int (*select_task_rq)(struct task_struct *p, int task_cpu, int sd_flag, int flags);
    void (*migrate_task_rq)(struct task_struct *p);
    void (*task_woken) (struct rq *this_rq, struct task_struct* task);
    void (*set_cpus_allowed)(struct task_struct* p, const struct cpumask *newmask);
    void (*rq_online)(struct rq *rq);
    void (*rq_offline)(struct rq *rq);
#endif
    
    void (*set_curr_task) (struct rq *rq);
    void (*task_tick) (struct rq *rq, struct task_struct* p, int queued);
    void (*task_fork) (struct task_struct* p);
    void (*task_dead) (struct task_struct* p);
    
    /*
     * The switched_from() call is allowed to drop rq->lock, therefore we
     * cannot assume the switched_from/switched_to pair is serialized by
     * rq->lock. They are however serialized by p->pi_lock.
     */
    void (*switched_from) (struct rq *this_rq, struct task_struct* task);
    void (*switched_to) (struct rq* this_rq, struct task_struct *task);
    void (*prio_changed) (struct rq *this_rq, struct task_struct* task, int oldprio);
    unsigned int (*get_rr_interval) (struct rq* rq, struct task_struct *task);
    void (*update_curr) (struct rq *rq);
    
#define TASK_SET_GROUP 0
#define TASK_MOVE_GROUP 1
#ifdef CONFIG_FAIR_GROUP_SCHED
    void (*task_change_group) (struct task_struct *p, int type);
#endif
};
```

### Current Scheduling Classes

As of the 4.12.x kernel, there are three scheduling classes:
1. **Completely Fair Scheduling (CFS) class**
2. **Real-Time Scheduling class** 
3. **Deadline Scheduling class**

Each class handles processes with specific scheduling requirements.

## Runqueue

Conventionally, the runqueue contains all the processes that are contending for CPU time on a given CPU core (a runqueue is per-CPU). The generic scheduler is designed to look into the runqueue whenever it is invoked to schedule the next best runnable task.

### Abstract Runqueue Design

Maintaining a common runqueue for all the runnable processes would not be possible since each scheduling class deals with specific scheduling policies and priorities. The kernel addresses this by bringing its design principles to the fore:

- Each scheduling class defines the layout of its runqueue data structure as best suitable for its policies
- The generic scheduler layer implements an abstract runqueue structure with common elements
- This structure is extended with pointers that refer to class-specific runqueues
- All scheduling classes embed their runqueues into the main runqueue structure

```c
struct rq {
    /* runqueue lock: */
    raw_spinlock_t lock;
    
    /*
     * nr_running and cpu_load should be in the same cacheline because
     * remote CPUs use both these fields when doing load calculation.
     */
    unsigned int nr_running;
    
#ifdef CONFIG_NUMA_BALANCING
    unsigned int nr_numa_running;
    unsigned int nr_preferred_running;
#endif
    
#define CPU_LOAD_IDX_MAX 5
    unsigned long cpu_load[CPU_LOAD_IDX_MAX];
    
    /* capture load from *all* tasks on this cpu: */
    struct load_weight load;
    unsigned long nr_load_updates;
    u64 nr_switches;
    
    struct cfs_rq cfs;
    struct rt_rq rt;
    struct dl_rq dl;
    
    unsigned long nr_uninterruptible;
    struct task_struct* curr, *idle,* stop;
    unsigned long next_balance;
    struct mm_struct *prev_mm;
    unsigned int clock_skip_update;
    
    u64 clock;
    u64 clock_task;
    
    atomic_t nr_iowait;
    
    /* calc_load related fields */
    unsigned long calc_load_update;
    long calc_load_active;
    
    /* ... other fields ... */
};
```

Key elements of the runqueue:
- **nr_running**: Number of processes in the runqueue
- **load**: Current load on the queue (all runnable processes)
- **curr** and **idle**: Point to the task_struct of the current running task and the idle task, respectively

## The Scheduler's Entry Point

The process of scheduling starts with a call to the generic scheduler, that is, the `schedule()` function, defined in `<kernel/sched/core.c>`. This is perhaps one of the most invoked routines in the kernel. 

The functionality of `schedule()` is to pick the next best runnable task. The `pick_next_task()` of the `schedule()` function iterates through all the corresponding functions contained in the scheduler classes and ends up picking the next best task to run.

### Optimization for Interactive Systems

Considering that Linux was primarily designed to cater to highly interactive systems, the function first looks for the next best runnable task in the CFS class if there are no higher-priority runnable tasks in any of the other classes. This is done by checking whether the total number of runnable tasks (`nr_running`) in the runqueue is equal to the total number of runnable tasks in the CFS class's sub-runqueue.

```c
/*
 * Pick up the highest-prio task:
 */
static inline struct task_struct*
pick_next_task(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    const struct sched_class* class;
    struct task_struct *p;
    
    /*
     * Optimization: we know that if all tasks are in the fair class we can
     * call that function directly, but only if the @prev task wasn't of a
     * higher scheduling class, because otherwise those loose the
     * opportunity to pull in more work from other CPUs.
     */
    if (likely((prev->sched_class == &idle_sched_class ||
                prev->sched_class == &fair_sched_class) &&
               rq->nr_running == rq->cfs.h_nr_running)) {
        
        p = fair_sched_class.pick_next_task(rq, prev, rf);
        if (unlikely(p == RETRY_TASK))
            goto again;
        
        /* Assumes fair_sched_class->next == idle_sched_class */
        if (unlikely(!p))
            p = idle_sched_class.pick_next_task(rq, prev, rf);
        
        return p;
    }
    
again:
    for_each_class(class) {
        p = class->pick_next_task(rq, prev, rf);
        if (p) {
            if (unlikely(p == RETRY_TASK))
                goto again;
            return p;
        }
    }
    
    /* The idle class should always have a runnable task: */
    BUG();
}
```

## Process Priorities

The decision of which process to run depends on the priority of the process. Every process is labelled with a priority value, giving it an immediate position in terms of when it will be given CPU time.

### Priority Types

Priorities are fundamentally classified into:

1. **Dynamic priorities**: Applied to normal processes dynamically by the kernel, considering various factors such as:
   - Nice value of the process
   - Historic behavior (I/O bound or processor bound)
   - Lapsed execution and waiting time

2. **Static priorities**: Applied to real-time processes by the user; the kernel does not change their priorities dynamically. Processes with static priorities are given higher priority when scheduling.

### Process Types

**I/O bound process**: When the execution of a process is heavily punctuated with I/O operations (waiting for a resource or an event). Examples include text editors, which almost alternate between running and waiting for key presses. Due to this nature, the scheduler normally allocates short processor time slices to I/O-bound processes and multiplexes them with other processes.

**Processor bound process**: These are processes that love to stick on to CPU time slices, as they require maximum utilization of the processor's computing capacity. Examples include complex scientific calculations and video rendering codecs. Though the need for a longer CPU slice looks desirable, the expectation to run them under fixed time periods is not often a requirement.

### Priority Values

From the user perspective:
- **Nice value** of any normal process: ranges between 19 (lowest priority) and -20 (highest priority), with 0 being the default
- **Real-time processes**: prioritized between 0 and 99 (static priority)
- Higher nice value indicates lower priority (the process is being "nicer" to other processes)

### Kernel's Perspective of Priorities

Linux scales all priorities between 0 to 139:
- **0 to 99**: Assigned for real-time processes  
- **100 to 139**: Represents the nice value range (-20 to 19)

## Scheduler Classes

### Key sched_class Elements

- **enqueue_task**: Adds a new process to the run queue
- **dequeue_task**: When the process is taken off the runqueue  
- **yield_task**: When the process wants to relinquish CPU voluntarily
- **pick_next_task**: Picks up the next best runnable task from its class

## Completely Fair Scheduling Class (CFS)

All processes with dynamic priorities are handled by the CFS class. As most processes in general-purpose *nix systems are normal (non-realtime), CFS remains the busiest scheduler class in the kernel.

### Core Philosophy

CFS relies on maintaining balance in allocating processor time to tasks, based on policies and dynamic priorities assigned per task. Process scheduling under CFS is implemented under the premise that it has an "ideal, precise multi-tasking CPU," that equally powers all processes at its peak capacity.

For example, if there are two processes, the perfectly multi-tasking CPU ensures that both processes run simultaneously, each utilizing 50% of its power. As this is practically impossible (achieving parallelism), CFS allocates processor time to a process by maintaining proper balance across all contending processes.

### Virtual Runtime (vruntime)

CFS does not rely on the traditional time slices for allocating processor time, but rather uses a concept of **virtual runtime (vruntime)**:

- Denotes the amount of time a process got CPU time
- Low vruntime value indicates that the process is processor deprived  
- High vruntime value denotes that the process acquired considerable processor time
- Processes with low vruntime values get maximum priority when scheduling

### Sleeper Fairness

CFS also engages sleeper fairness for processes that are ideally waiting for an I/O request. Sleeper fairness demands that waiting processes be given considerable CPU time when they eventually wake up, post event.

### Weight and Nice Values

CFS uses the nice value to weigh a process in relation to all contending processes:
- Higher-value, low-priority process gets less weight
- Lower-value, high-priority task gets more weight
- Lower-priority task gets considerable factors of delay compared to higher-priority task

### Computing Priorities and Time Slices Under CFS

Priorities are assigned based on:
- How long the process is waiting
- How long the process ran  
- The process's historical behavior
- Its nice value

#### Load Weight Calculation

For every jump in the nice value of a process by 1:
- 10% reduction in the CPU timeslice (nice value increase)
- 10% addition in the CPU timeslice (nice value decrease)

The kernel maintains an array called `prio_to_weight`, where each nice value corresponds to a weight:

```c
static const int prio_to_weight[40] = {
    /* -20 */     88761,     71755,     56483,     46273,     36291,
    /* -15 */     29154,     23254,     18705,     14949,     11916,
    /* -10 */      9548,      7620,      6100,      4904,      3906,
    /*  -5 */      3121,      2501,      1991,      1586,      1277,
    /*   0 */      1024,       820,       655,       526,       423,
    /*   5 */       335,       272,       215,       172,       137,
    /*  10 */       110,        87,        70,        56,        45,
    /*  15 */        36,        29,        23,        18,        15,
};
```

The timeslice is computed by factoring:
- Entity's load weight
- Runqueue's load weight  
- The sched_period (scheduling period)

### CFS's Runqueue

CFS sheds the need for a normal runqueue and uses a **self-balancing, red-black tree** instead to get to the next best process to run in the shortest possible time. 

- The RB tree holds all the contending processes
- Facilitates easy and quick insertion, deletion, and searching of processes
- The highest-priority process is placed at its leftmost node
- The `pick_next_task()` function picks the leftmost node from the rb tree to schedule

### Group Scheduling

To ensure fairness when scheduling, CFS is designed to guarantee that every runnable process gets at least one run on the processor under a defined time duration, called the **scheduling period**.

#### The Problem

Imagine a scenario where:
- Process A spawns 10 threads of execution
- Process B spawns 5 threads of execution

CFS divides timeslices to all threads equally, leading to process A and its spawned threads getting maximum time and process B being dealt with unfairly.

#### The Solution

CFS introduced **group scheduling**, where timeslices are allotted to groups of threads instead of individual threads:

- Process A and its spawned threads belong to one group
- Process B and its spawned threads belong to another group
- Scheduling granularity is imposed at group level, not thread level
- Groups get equal share of processor time, then divide among group members internally

To ensure group scheduling, `CONFIG_FAIR_GROUP_SCHED` must be set when configuring the kernel.

#### Scheduling Entity Structure

CFS task groups are represented by the structure `sched_entity`:

```c
struct sched_entity {
    struct load_weight load;        /* for load-balancing */
    struct rb_node run_node;
    struct list_head group_node;
    unsigned int on_rq;
    
    u64 exec_start;
    u64 sum_exec_runtime;
    u64 vruntime;
    u64 prev_sum_exec_runtime;
    u64 nr_migrations;
    
#ifdef CONFIG_SCHEDSTATS
    struct sched_statistics statistics;
#endif
    
#ifdef CONFIG_FAIR_GROUP_SCHED
    int depth;
    struct sched_entity *parent;
    /* rq on which this entity is (to be) queued: */
    struct cfs_rq *cfs_rq;
    /* rq "owned" by this entity/group: */
    struct cfs_rq *my_q;
#endif
};
```

Key fields:
- **load**: Amount of load each entity bears on the total load of the queue
- **vruntime**: Amount of time the process ran

### Scheduling Entities Under Many-Core Systems

Task groups can run on any CPU core, but creating only one scheduling entity will not suffice. Groups must create a scheduling entity for every CPU core on the system.

Scheduling entities across CPUs are represented by `struct task_group`:

```c
struct task_group {
    struct cgroup_subsys_state css;
    
#ifdef CONFIG_FAIR_GROUP_SCHED
    /* schedulable entities of this group on each cpu */
    struct sched_entity **se;
    /* runqueue "owned" by this group on each cpu */
    struct cfs_rq **cfs_rq;
    unsigned long shares;
    
#ifdef CONFIG_SMP
    /*
     * load_avg can be heavily contended at clock tick time, so put
     * it in its own cacheline separated from the fields above which
     * will also be accessed at each tick.
     */
    atomic_long_t load_avg ____cacheline_aligned;
#endif
#endif
    
    /* ... other fields ... */
};
```

### CFS Scheduling Policies

The CFS class handles processes using the following policies:

**SCHED_NORMAL (0)**: Used for all normal processes. All non-realtime processes can be summarized as normal processes. Most of the scheduling activity and heuristics are centered to fairly schedule normal processes. Normal processes are referred to as `SCHED_OTHER` as per POSIX.

**SCHED_BATCH (3)**: Used in servers where processes are non-interactive, CPU-bound batch processing. These CPU-intensive processes are given less priority than a `SCHED_NORMAL` process, and they do not preempt normal processes.

**SCHED_IDLE (5)**: When there are no processes to run, the idle process (low-priority background processes) is scheduled. The idle process is assigned the least priority among all processes.

## Real-Time Scheduling Class

Linux supports soft real-time tasks and they are scheduled by the real-time scheduling class. RT processes are assigned static priorities and are unchanged dynamically by the kernel.

### Key Characteristics

- Real-time tasks aim at deterministic runs and desire control over when and how long they are to be scheduled
- They are always given preference over normal tasks (`SCHED_NORMAL`)
- Unlike CFS, which uses rb tree as its subrunqueue, the rt scheduler uses a simple linked list per priority value (1 to 99)
- Less complicated than CFS

### Real-Time Policies

Linux applies two real-time policies when scheduling static priority processes:

**SCHED_FIFO (1)**: Uses the first in, first out method to schedule soft real-time processes

**SCHED_RR (2)**: Uses the round-robin policy to schedule soft real-time processes

### FIFO Scheduling

FIFO is a scheduling mechanism applied to processes with priorities higher than 0:

- FIFO processes run without any timeslice allocation
- They invariably run until they block for some event or explicitly yield to another process
- A FIFO process gets preempted when the scheduler encounters a higher-priority runnable FIFO, RR, or deadline task
- When scheduler encounters more than one fifo task with the same priority, it runs the processes in round robin
- On preemption, the process is added back to the tail of the list
- If a higher-priority process preempts the FIFO process, it waits at the head of the list
- When a new fifo process becomes runnable, it is added to the tail of the list

### Round-Robin (RR) Scheduling

The round-robin policy is similar to FIFO, with the only exception being that it is allocated a timeslice to run:

- This is an enhancement to FIFO (as a FIFO process may run until it yields or waits)
- Similar to FIFO, the RR process at the head of the list is picked for execution
- On completion of the timeslice, it gets preempted and is added back to the tail end of the list
- RR processes with the same priority run round robin until preempted by a high-priority task
- When a high-priority task preempts an RR task, it waits at the head of the list
- On resumption, runs for the remainder of its timeslice only

### Real-Time Group Scheduling

Similar to group scheduling under CFS, real-time processes can also be grouped for scheduling with `CONFIG_RT_GROUP_SCHED` set.

For group scheduling to succeed:
- Each group must be assigned a portion of CPU time
- Guarantee that the timeslice is enough to run the tasks under each entity, or it fails
- "Run time" (portion of how much time a CPU can spend running in a period) is allocated per group
- Run time allocated to one group will not be used by another group
- CPU time not allocated for realtime groups will be used by normal-priority tasks
- Unused time by real-time entities will also be picked by normal tasks

FIFO and RR groups are represented by `struct sched_rt_entity`:

```c
struct sched_rt_entity {
    struct list_head run_list;
    unsigned long timeout;
    unsigned long watchdog_stamp;
    unsigned int time_slice;
    unsigned short on_rq;
    unsigned short on_list;
    
    struct sched_rt_entity *back;
    
#ifdef CONFIG_RT_GROUP_SCHED
    struct sched_rt_entity *parent;
    /* rq on which this entity is (to be) queued: */
    struct rt_rq *rt_rq;
    /* rq "owned" by this entity/group: */
    struct rt_rq *my_q;
#endif
};
```

## Deadline Scheduling Class (Sporadic Task Model Deadline Scheduling)

Deadline represents the new breed of RT processes on Linux (added since the 3.14 kernel). Unlike FIFO and RR, where processes may hog CPU or be bound by timeslices, a deadline process predetermines its runtime requirements.

### Key Features

- Based on **GEDF (Global Earliest Deadline First)** and **CBS (Constant Bandwidth Server)** algorithms
- A sporadic process internally runs multiple tasks
- Each task has a relative deadline within which it must complete executing
- Each task has a computation time, defining the time that the CPU needs to complete process execution
- To ensure that the kernel succeeds in executing deadline processes, the kernel runs an **admittance test** based on the deadline parameters
- On failure, returns an error `EBUSY`
- Processes with the deadline policy get precedence over all other processes
- Deadline processes use **SCHED_DEADLINE (6)** as their policy element

## Scheduler Related System Calls

Linux provides an entire family of system calls that manage various scheduler parameters, policies, and priorities and retrieve scheduling-related information for threads.

### Priority Management

**nice(int inc)**: Takes an int parameter and adds it to the nice value of the calling thread. Returns the new nice value on success. Nice values are within the range 19 (lowest priority) to -20 (highest priority).

**getpriority(int which, id_t who)**: Returns the nice value of the thread, group, user, or set of threads as indicated by its parameters. Returns the highest priority held by any of the processes.

**setpriority(int which, id_t who, int prio)**: Sets the scheduling priority of the thread, group, user, or set of threads as indicated by its parameters. Returns zero on success.

### Scheduler Policy Management

**sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)**: Sets both the scheduling policy and parameters of a specified thread. The param argument specifies the scheduling parameters. Returns zero on success.

**sched_getscheduler(pid_t pid)**: Returns the scheduling policy of a thread (pid). If the pid is zero, the policy of the calling thread will be retrieved.

**sched_setparam(pid_t pid, const struct sched_param* param)**: Sets the scheduling parameters associated with the scheduling policy of the given thread. Returns zero on success.

**sched_getparam(pid_t pid, struct sched_param *param)**: Gets the scheduling parameters for the specified thread. Returns zero on success.

**sched_setattr(pid_t pid, struct sched_attr* attr, unsigned int flags)**: Linux-specific call that sets the scheduling policy and related attributes for the specified thread. This is a superset of the functionality provided by `sched_setscheduler()` and `sched_setparam()` calls.

**sched_getattr(pid_t pid, struct sched_attr *attr, unsigned int size, unsigned int flags)**: Linux-specific call that fetches the scheduling policy and related attributes of the specified thread.

### Priority Limits

**sched_get_priority_max(int policy)** / **sched_get_priority_min(int policy)**: Returns the max and min priority respectively for the specified policy. FIFO, RR, deadline, normal, batch, and idle are supported values of policy.

**sched_rr_get_interval(pid_t pid, struct timespec* tp)**: Fetches the time quantum of the specified thread and writes it into the timespec struct. Only applicable to processes with the RR policy.

### CPU Control

**sched_yield(void)**: Called to relinquish the CPU explicitly. The thread is added back to the queue. Returns zero on success.

### Processor Affinity Calls

Linux-specific processor affinity calls help threads define on which CPU(s) they want to run:

**sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask)**: Sets the CPU affinity mask of the thread to the value mentioned by mask. If the thread is not running in one of the specified CPU's queues, it is migrated to the specified cpu.

**sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask)**: Fetches the affinity mask of the thread into the cpusetsize structure, pointed to by mask.

The affinity bitmask information is contained in the `cpu_allowed` field of `struct task_struct`.

## Process Preemption

Understanding preemption and context switching is key to fully comprehending scheduling and the impact it has on the kernel in maintaining low latency and consistency.

### Context Switching

Every process must be preempted either implicitly or explicitly to make way for another process. Preemption might lead to context switching, which requires a low-level architecture-specific operation, carried out by the function `context_switch()`.

Two primary tasks need to be done for a processor to switch its context:
1. **Switch the virtual memory mapping** of the old process with the new one
2. **Switch the processor state** from that of the old process to the new one

These two tasks are carried out by `switch_mm()` and `switch_to()`.

### Preemption Triggers

Preemption can happen for any of the following reasons:

1. **When a high-priority process becomes runnable**: The scheduler periodically checks for high-priority runnable threads
2. **On return from interrupts and system calls**: `TIF_NEED_RESCHEDULE` flag is set, invoking the scheduler
3. **Periodic timer interrupt**: Guaranteed to occur at regular intervals, ensuring scheduler invocation
4. **When a process enters a blocking call**
5. **On occurrence of an interrupt event**

### Kernel Preemption

The Linux kernel historically has been non-preemptive, meaning a task in kernel mode is non-preemptible unless an interrupt event occurs or it chooses to explicitly relinquish CPU.

#### Preemptible Kernel (since 2.6)

With kernel preemption enabled (needs to be enabled during kernel build):
- A task in kernel mode is preemptible for all the reasons listed
- A kernel-mode task is allowed to disable kernel preemption while carrying out critical operations
- This is made possible by adding a **preemption counter** (`preempt_count`) to each process's `thread_info` structure

#### Preemption Control

Tasks can disable/enable preemption through kernel macros:
- `preempt_disable()`: Increments the preempt_counter
- `preempt_enable()`: Decrements the preempt_counter

This ensures that the kernel is preemptible only when the `preempt_counter` is zero (indicating no acquired locks).

#### Critical Sections

Critical sections in the kernel code are executed by disabling preemption, which is enforced by invoking `preempt_disable` and `preempt_enable` calls within kernel lock operations (spinlock, mutex).

#### Fully Preemptible Kernel

Linux kernels built with "preempt rt", supporting fully preemptible kernel option, make all the kernel code including critical sections fully preemptible when enabled.

## Summary

Process scheduling is an ever-evolving aspect of the kernel, and as Linux evolves and diversifies further into many computing domains, finer tweaks and changes to the process scheduler will be mandated. However, with our understanding established over this chapter, gaining deeper insights or comprehending any new changes will be quite easy.

The key takeaways from this chapter include:

1. **Two-Layered Design**: Generic scheduler layer with pluggable scheduling classes
2. **Three Main Classes**: CFS (normal processes), RT (real-time processes), and Deadline (sporadic real-time processes)
3. **CFS Innovation**: Virtual runtime and red-black tree for fair, efficient scheduling
4. **Group Scheduling**: Fair resource allocation among process groups
5. **Real-Time Support**: FIFO and Round-Robin policies for deterministic execution
6. **Preemption Control**: Configurable kernel preemption for different system requirements

This scheduler design allows Linux to efficiently handle diverse workloads from interactive desktop systems to real-time embedded systems, making it truly versatile across different computing domains.