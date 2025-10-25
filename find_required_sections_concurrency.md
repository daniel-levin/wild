# Concurrency in `find_required_sections`

## Overview

The `find_required_sections` function (`layout.rs:2146-2256`) implements a sophisticated work-stealing concurrency pattern for parallel symbol resolution and section loading during the linking process. This document explains how the concurrency mechanisms work.

## Core Components

### 1. Data Structures

**`GraphResources<'data, 'scope>`** (`layout.rs:1478-1516`)
```
┌─────────────────────────────────────────────────────────────────┐
│                         GraphResources                           │
│  (Shared across all threads)                                     │
├─────────────────────────────────────────────────────────────────┤
│  • worker_slots: Vec<Mutex<WorkerSlot>>              (1481)     │
│  • waiting_workers: ArrayQueue<GroupState>           (1485)     │
│  • idle_threads: Option<ArrayQueue<Thread>>          (1492)     │
│      - size: N-1  (see line 2159)                               │
│  • done: AtomicBool                                  (1494)     │
│  • errors: Mutex<Vec<Error>>                         (1483)     │
│  • symbol_db, per_symbol_flags, sections_with_content, etc.     │
└─────────────────────────────────────────────────────────────────┘

┌──────────────────────────┐      ┌──────────────────────────┐
│  WorkerSlot (2133-2136)  │      │  GroupState (1457-1462)  │
├──────────────────────────┤      ├──────────────────────────┤
│  work: Vec<WorkItem>     │      │  queue: LocalWorkQueue   │
│  worker: Option<Group    │      │  files: Vec<FileLayout>  │
│          State>          │      │  common: CommonGroupState│
└──────────────────────────┘      └──────────────────────────┘
         │                                    │
         │                                    │
         └────────────────┬───────────────────┘
                          │
                 Worker may be "parked" in
                 slot or "active" in queue
```

### 2. Work Items

Work items represent units of work that need to be processed (`layout.rs:1529-1545`):

```rust
enum WorkItem {
    LoadGlobalSymbol(SymbolId),      // Load symbol and process relocations
    CopyRelocateSymbol(SymbolId),    // Create copy relocation in BSS
    LoadSection(SectionLoadRequest), // Load specific section
    ExportDynamic(SymbolId),         // Export as dynamic symbol
}
```

**`LocalWorkQueue`** (`layout.rs:1373-1379`) routes work based on group ownership:
- Same group → push to `local_work` (line 2426)
- Different group → call `GraphResources::send_work()` (line 2428)

### Key Functions Referenced

- `create_worker_slots()` - Initialize worker infrastructure (`layout.rs:2258-2287`)
- `activate()` - Generate initial work items from files (`layout.rs:2405-2420`)
- `GroupState::do_pending_work()` - Main worker processing loop (`layout.rs:2301-2322`)
- `GraphResources::send_work()` - Cross-group work distribution (`layout.rs:2469-2489`)
- `GraphResources::shut_down()` - Graceful termination (`layout.rs:2491-2499`)

## Concurrency Architecture

### Phase 1: Initialization (`layout.rs:2154-2190`)

```
Initial State:
┌──────────────────────────────────────────────────────────────┐
│  Main Thread                                                  │
│                                                               │
│  1. Create WorkerSlots (one per group)         (2155, 2258)  │
│     ┌────────┐  ┌────────┐  ┌────────┐                     │
│     │ Slot 0 │  │ Slot 1 │  │ Slot N │                     │
│     └────────┘  └────────┘  └────────┘                     │
│                                                               │
│  2. Create GroupStates (workers)               (2155, 2264)  │
│     ┌────────┐  ┌────────┐  ┌────────┐                     │
│     │Group 0 │  │Group 1 │  │Group N │                     │
│     └────────┘  └────────┘  └────────┘                     │
│                                                               │
│  3. Create idle_threads queue                  (2159)        │
│     capacity: num_threads - 1                                │
│     ArrayQueue: [empty] [empty] [empty]                     │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

The code uses Rayon's `par_iter()` to initialize workers in parallel (lines 2179-2190):

```rust
groups
    .into_par_iter()
    .enumerate()
    .try_for_each(|(i, mut group)| -> Result {
        // Activate initial files in each group (2184-2186)
        for file in &mut group.files {
            activate::<A>(&mut group.common, file, &mut group.queue, resources_ref)?;
        }
        // Place worker in waiting queue (2188)
        let _ = resources_ref.waiting_workers.push(group);
        Ok(())
    })?;
```

**Key Point**: Each group processes its files and generates initial work items (via `activate()` at line 2405), then places itself in `waiting_workers`.

### Phase 2: Work Processing (`layout.rs:2192-2228`)

This is where the sophisticated concurrency happens. The system uses `rayon::scope` with `spawn_broadcast` (line 2193) to create N threads that all run the same work-stealing loop.

```
Thread Lifecycle:
┌─────────────────────────────────────────────────────────────┐
│                    Rayon Thread Pool                         │
│  (N threads, all running same code via spawn_broadcast)     │
└─────────────────────────────────────────────────────────────┘
          │         │         │         │
          ▼         ▼         ▼         ▼
      Thread 0  Thread 1  Thread 2  Thread N-1
          │         │         │         │
          └─────────┴─────────┴─────────┘
                    │
          All execute same loop:
                    │
    ┌───────────────▼────────────────┐
    │  while !resources.done {       │
    │    1. Check waiting_workers    │
    │    2. Process work if found    │
    │    3. Try to go idle           │
    │    4. Park if not last thread  │
    │  }                             │
    └────────────────────────────────┘
```

#### The Work-Stealing Loop

Each thread runs this loop (`layout.rs:2196-2220`):

```
┌──────────────────────────────────────────────────────────────┐
│ Thread Execution Loop                                         │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌─────────────────────────────────────┐                    │
│  │  while !done.load() {               │◄───────┐           │
│  │                                      │        │ Loop      │
│  │    ┌──────────────────────────┐     │        │           │
│  │    │ Pop from waiting_workers  │     │        │           │
│  │    └──────────┬───────────────┘     │        │           │
│  │               │                      │        │           │
│  │               ▼                      │        │           │
│  │         Found worker?  (2197)       │        │           │
│  │          ╱         ╲                 │        │           │
│  │        Yes          No               │        │           │
│  │         │            │               │        │           │
│  │         ▼            ▼               │        │           │
│  │   Do work      Check idle state     │        │           │
│  │   (2198)       (2200-2219)          │        │           │
│  │         │            │               │        │           │
│  │         └────────────┘               │        │           │
│  │                │                     │        │           │
│  └────────────────┼─────────────────────┘        │           │
│                   │                              │           │
│                   └──────────────────────────────┘           │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

#### Idle Detection and Termination

The brilliant part is the idle detection mechanism:

```
Idle Thread Management:
────────────────────────────────────────────────────────────────

State 1: Thread has work
┌─────────────────────────────────────────────────────┐
│  idle = false                                        │
│  Thread processes waiting_workers queue             │
└─────────────────────────────────────────────────────┘

State 2: No work found (first time)                   (2204-2215)
┌─────────────────────────────────────────────────────┐
│  idle = false → true                     (2215)     │
│  Try to push current thread to idle_threads (2205)  │
│                                                      │
│  Case A: Queue has space                 (2206 ok)  │
│    → Thread added to idle_threads                   │
│    → Loop again (don't park yet!)        (2216-18)  │
│                                                      │
│  Case B: Queue is FULL (all other threads idle)     │
│           (2206 returns Err)                        │
│    → This thread is the LAST active thread          │
│    → Call shut_down()                    (2212)     │
│    → Break                               (2213)     │
└─────────────────────────────────────────────────────┘

State 3: Still no work (second iteration)             (2200-2203)
┌─────────────────────────────────────────────────────┐
│  idle = true                                         │
│  std::thread::park() - put thread to sleep  (2202)  │
│  (Will be woken by unpark() when work arrives)      │
└─────────────────────────────────────────────────────┘

Key Insight:
────────────
The idle_threads queue has capacity (N-1). When the Nth thread
tries to add itself and fails, it knows ALL threads are idle
→ work is complete → shutdown.
```

### Phase 3: Cross-Group Work Distribution

When a worker processes symbols that belong to different groups:

```
Work Distribution Flow:
────────────────────────────────────────────────────────

Thread A processing Group 0:
┌─────────────────────────────────────────────────────┐
│  Symbol X belongs to Group 0 (same group)           │
│    → Push to local_work queue           (2426)      │
│    → Process immediately in next iteration          │
└─────────────────────────────────────────────────────┘

Thread A processing Group 0:
┌─────────────────────────────────────────────────────┐
│  Symbol Y belongs to Group 2 (different group)      │
│    │                                                 │
│    ▼                                                 │
│  Call resources.send_work(file_id, work_item) (2428)│
└────────────┬────────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────────────────────┐
│  GraphResources::send_work()  (layout.rs:2469-2488)   │
│  ────────────────────────────────────────────────────  │
│                                                         │
│  1. Lock worker_slots[Group2]                  (2472)  │
│     ┌─────────────────────────────┐                   │
│     │ let mut slot = ...lock()    │                   │
│     └─────────────────────────────┘                   │
│                                                         │
│  2. Take worker (if present)                   (2473)  │
│     ┌─────────────────────────────┐                   │
│     │ worker = slot.worker.take() │                   │
│     └─────────────────────────────┘                   │
│                                                         │
│  3. Push work to slot                          (2474)  │
│     ┌─────────────────────────────┐                   │
│     │ slot.work.push(work_item)   │                   │
│     └─────────────────────────────┘                   │
│                                                         │
│  4. Release lock                               (2475)  │
│                                                         │
│  5. If worker was taken:                       (2476)  │
│     a. Push to waiting_workers                 (2479)  │
│     b. Unpark an idle thread (if any)          (2486)  │
│                                                         │
└────────────────────────────────────────────────────────┘

State Diagram of Worker Slot:
──────────────────────────────

  ┌─────────────────────────────┐
  │  Worker in Slot (Idle)      │
  │  worker: Some(GroupState)   │
  │  work: []                   │
  └──────────┬──────────────────┘
             │
             │ send_work() takes worker
             ▼
  ┌─────────────────────────────┐
  │  Slot with Work (No Worker) │
  │  worker: None               │
  │  work: [item1, item2, ...]  │
  └──────────┬──────────────────┘
             │
             │ Worker checks slot
             │ (line 2314-2320)
             ▼
  ┌─────────────────────────────┐
  │  Worker Active              │
  │  (Processing in thread)     │
  │  Periodically swaps work    │
  │  from slot to local queue   │
  └──────────┬──────────────────┘
             │
             │ Work complete
             ▼
  ┌─────────────────────────────┐
  │  Worker Returns to Slot     │
  │  worker: Some(GroupState)   │
  │  work: []                   │
  └─────────────────────────────┘
```

### Phase 4: Worker Processing Loop

When a worker is active, it processes work in `GroupState::do_pending_work()` (`layout.rs:2301-2322`):

```
Worker Processing (do_pending_work):
──────────────────────────────────────────────────────────

┌───────────────────────────────────────────────────────┐
│  loop {                                        (2302)  │
│                                                         │
│    ┌─────────────────────────────────────────┐       │
│    │ Phase A: Process Local Queue    (2303)  │       │
│    │ ─────────────────────────────            │       │
│    │                                           │       │
│    │  while let Some(work) = queue.pop() {    │       │
│    │      do_work::<A>(work_item)     (2307)  │       │
│    │  }                                        │       │
│    │                                           │       │
│    │  This may generate MORE work items:      │       │
│    │  - Same group → local_work queue         │       │
│    │  - Other group → send_work()             │       │
│    └─────────────────────────────────────────┘       │
│                                                         │
│    ┌─────────────────────────────────────────┐       │
│    │ Phase B: Check for Cross-Group Work     │       │
│    │ ───────────────────────────────────      │       │
│    │                                           │       │
│    │  Lock my worker_slot                     │       │
│    │                                   (2314)  │       │
│    │  if slot.work.is_empty() {       (2315)  │       │
│    │    // No new work, I'm done              │       │
│    │    slot.worker = Some(self)      (2316)  │       │
│    │    return                        (2317)  │       │
│    │  }                                        │       │
│    │                                           │       │
│    │  // New work arrived!                    │       │
│    │  swap(&mut slot.work,            (2319)  │       │
│    │       &mut queue.local_work)             │       │
│    │                                           │       │
│    │  Unlock slot                     (2320)  │       │
│    └─────────────────────────────────────────┘       │
│                                                         │
│  } // Loop back to Phase A                            │
└───────────────────────────────────────────────────────┘

Key Insight:
────────────
Workers batch-swap work from their slot to minimize lock contention.
This allows other threads to deposit work while the worker processes
its current batch.
```

## Complete Execution Flow Example

Let's trace a concrete example with 3 threads and 4 groups:

```
Initial Setup:
──────────────
Threads: T0, T1, T2
Groups: G0, G1, G2, G3
idle_threads capacity: 2 (num_threads - 1)

waiting_workers: [G0, G1, G2, G3]
idle_threads: []

Step 1: Threads grab workers
─────────────────────────────
T0: Pop G0 from waiting_workers → process G0
T1: Pop G1 from waiting_workers → process G1
T2: Pop G2 from waiting_workers → process G2

waiting_workers: [G3]
idle_threads: []

Step 2: G0 generates cross-group work for G2
─────────────────────────────────────────────
T0 processing G0:
  - Encounters symbol in G2
  - Calls send_work(G2_file_id, work_item)

send_work() execution:
  1. Lock worker_slots[2]
  2. slot.worker = None (G2 is active in T2)
  3. slot.work.push(work_item)
  4. Unlock
  5. worker was None, so don't push to waiting_workers
  6. No idle threads to wake

Result: Work queued in G2's slot, will be picked up by T2

Step 3: T0 finishes G0, T2 still working
─────────────────────────────────────────
T0 finishes G0:
  - do_pending_work() returns
  - G0 is back in its slot with worker = Some(G0)

T0 loop iteration:
  - Pop G3 from waiting_workers → process G3

waiting_workers: []
idle_threads: []

Step 4: T1 finishes G1, tries to go idle
─────────────────────────────────────────
T1 finishes G1:
  - do_pending_work() returns
  - G1 back in slot

T1 loop iteration:
  - Try pop from waiting_workers → None
  - idle = false, try to add to idle_threads
  - idle_threads.push(T1) → SUCCESS (capacity 2)
  - idle = true
  - Loop again (check for work before parking)
  - Still no work in waiting_workers
  - idle = true, so park()

waiting_workers: []
idle_threads: [T1]
Active threads: T0 (G3), T2 (G2)

Step 5: T2 finishes local work, finds slot work
────────────────────────────────────────────────
T2 in do_pending_work():
  - Local queue empty
  - Lock worker_slots[2]
  - slot.work is NOT empty (work from Step 2!)
  - swap(&mut slot.work, &mut queue.local_work)
  - Continue processing

Step 6: T0 finishes G3
──────────────────────
T0 finishes G3:
  - do_pending_work() returns
  - G3 back in slot

T0 loop iteration:
  - Try pop from waiting_workers → None
  - idle = false, try to add to idle_threads
  - idle_threads.push(T0) → SUCCESS (now at capacity 2)
  - idle = true
  - Loop again
  - Still no work
  - idle = true, so park()

waiting_workers: []
idle_threads: [T1, T0]
Active threads: T2 (G2)

Step 7: T2 finishes all work
─────────────────────────────
T2 finishes G2:
  - do_pending_work() returns
  - G2 back in slot

T2 loop iteration:
  - Try pop from waiting_workers → None
  - idle = false, try to add to idle_threads
  - idle_threads.push(T2) → FAILURE (queue full!)

  T2 realizes: "All other threads are idle, I'm the last one!"

  - Call shut_down():
      * Set done = true
      * Pop T1 from idle_threads → unpark(T1)
      * Pop T0 from idle_threads → unpark(T0)
  - Break from loop

Step 8: All threads wake and exit
──────────────────────────────────
T1: Wake up, check done == true, exit
T0: Wake up, check done == true, exit
T2: Already exited

Result: rayon::scope completes, all workers in slots
```

## Synchronization Primitives

### Lock-Free Structures

1. **ArrayQueue** (from `crossbeam`):
   - `waiting_workers`: Lock-free queue for workers ready to process
   - `idle_threads`: Lock-free queue storing parked threads
   - Multiple threads can push/pop concurrently without locks

2. **AtomicBool**:
   - `done`: Signals global shutdown
   - `has_static_tls`, `uses_tlsld`: Track boolean flags across threads

### Mutex-Protected Structures

1. **WorkerSlot** (`Vec<Mutex<WorkerSlot>>`):
   - Each slot has its own mutex
   - Fine-grained locking: threads only contend on specific groups
   - Critical section is minimal (push work + take worker)

2. **Errors** (`Mutex<Vec<Error>>`):
   - Centralized error collection
   - Rarely contended (errors are exceptional)

## Design Rationale

### Why This Architecture?

1. **Work Affinity**: Each group stays with its data, minimizing false sharing

2. **Minimal Lock Contention**:
   - Workers process local queues without locks
   - Cross-group communication uses per-group mutexes
   - Lock-free queues for coordination

3. **Automatic Load Balancing**:
   - Threads steal from `waiting_workers` dynamically
   - If one group generates lots of work, other threads help

4. **Zero Busy-Waiting**:
   - Idle threads park (OS-level sleep)
   - Woken only when work arrives or shutdown occurs

5. **Deadlock-Free**:
   - No circular locking (each worker has independent slot)
   - Lock-free queues prevent blocking on coordination

6. **Graceful Termination**:
   - Elegant idle detection using (N-1) sized queue
   - No polling or timeout needed
   - All threads wake for clean shutdown

## Potential Race Conditions (Handled)

### Race 1: Work arrives while going idle

```
Thread A (trying to idle):
  1. Check waiting_workers → empty
  2. Add to idle_threads
  3. Loop again (re-check) ← PROTECTS AGAINST RACE
  4. Park only if still no work
```

**Protection**: The code loops again after adding to idle_threads (lines 2216-2218, comment at 2216: "Go around the loop again before we park").

### Race 2: Multiple threads check shutdown simultaneously

```
Thread A, B both find no work:
  A: Try push to idle_threads → SUCCESS
  B: Try push to idle_threads → SUCCESS (if space)

Eventually one thread (C):
  C: Try push to idle_threads → FAILURE
  C: Calls shut_down()
```

**Protection**: Atomic operations on idle_threads queue ensure exactly one thread detects the full condition.

### Race 3: Work sent after worker checks slot

```
Worker in do_pending_work() (lines 2314-2320):
  1. Lock slot                   (2314)
  2. Check slot.work.is_empty()  (2315)
  3. If empty, return worker     (2316)
  4. Unlock                       (2320)

Sender in send_work() (lines 2472-2475) cannot add work
between steps 2-3 because the slot is locked!
```

**Protection**: Mutex ensures atomic check-and-return.

## Performance Characteristics

- **Best Case**: All groups have independent work → near-linear speedup
- **Worst Case**: All work in one group → other threads idle quickly
- **Lock Contention**: O(1) per work item for cross-group dependencies
- **Memory Overhead**: One slot per group + (N-1) thread handles
- **Cache Behavior**: Good locality (workers process same group data)

## Error Handling and Panic Safety

The system includes robust error handling:

1. **Error Collection**: Errors are collected in `Mutex<Vec<Error>>` (line 1483) via `report_error()` (line 2462) and checked after all threads complete (line 2230)

2. **Panic Handling** (`layout.rs:2194, 2222-2226`):
   ```rust
   let panic_result = std::panic::catch_unwind(|| { ... });  // 2194
   if panic_result.is_err() {                                 // 2224
       resources.shut_down();                                 // 2225
   }
   ```
   If any thread panics, `shut_down()` (line 2491) is called to wake all sleeping threads and set `done = true`, preventing indefinite waiting.

3. **Early Exit on Error**: Workers return immediately on error (lines 2309-2310), and the error is reported to the shared error vector.

## Summary

The `find_required_sections` concurrency model is a sophisticated work-stealing scheduler with:

1. **Group-based partitioning** for data locality
2. **Lock-free coordination** for worker management
3. **Elegant termination detection** using N-1 sized idle queue
4. **Minimal synchronization** overhead via local queues and batch swapping
5. **Automatic load balancing** through shared worker queue
6. **Robust error and panic handling** preventing thread starvation

This design efficiently parallelizes the complex dependency graph traversal required for linker symbol resolution and section loading.
