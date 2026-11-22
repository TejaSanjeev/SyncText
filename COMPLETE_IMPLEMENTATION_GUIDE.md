# SyncText: Complete Architecture & Implementation Guide

> **Project Name**: SyncText – A Decentralized, Lock-Free, CRDT-Based Collaborative Text Editor  
> **Platform**: Linux (Ubuntu 24.04 LTS, x86_64) | **Language**: C++17 | **Build Flags**: `-lpthread -lrt`  
> **Key Paradigms**: Operating Systems IPC, Shared Memory, Message Queues, Lock-Free Concurrency, CRDTs (LWW)

---

## Table of Contents
1. [The 30-Second Summary](#1-the-30-second-summary)
2. [High-Level Architecture & The Dual-IPC Model](#2-high-level-architecture--the-dual-ipc-model)
3. [Deep Operating Systems & Memory Architecture](#3-deep-operating-systems--memory-architecture)
4. [Lock-Free Programming & Atomic Primitives](#4-lock-free-programming--atomic-primitives)
5. [Module-by-Module Code Walkthrough](#5-module-by-module-code-walkthrough)
   - [5.1 update.h (Data Contract)](#51-updateh-data-contract)
   - [5.2 registry.cpp (Lock-Free Peer Discovery)](#52-registrycpp-lock-free-peer-discovery)
   - [5.3 messaging.cpp (POSIX Message Queues)](#53-messagingcpp-posix-message-queues)
   - [5.4 crdt_merge.h (CRDT Merge Engine)](#54-crdt_mergeh-crdt-merge-engine)
   - [5.5 global_broadcast.cpp (Batching & Fan-Out)](#55-global_broadcastcpp-batching--fan-out)
   - [5.6 file_monitor.cpp (Local Loop & Echo Neutralizer)](#56-file_monitorcpp-local-loop--echo-neutralizer)
   - [5.7 main.cpp (Thread Orchestration)](#57-maincpp-thread-orchestration)
6. [The "Infinite Echo Loop" & Its Solution](#6-the-infinite-echo-loop--its-solution)
7. [Comprehensive Questions & Answers](#7-comprehensive-questions--answers)
8. [Compilation, Execution & Testing Guide](#8-compilation-execution--testing-guide)

---

## 1. The 30-Second Summary

> *"SyncText is a decentralized, lock-free collaborative text editor written in C++17 for Linux. It allows multiple concurrent processes to edit local text files simultaneously without a central server. It utilizes POSIX Shared Memory for peer discovery and global batching, POSIX Message Queues for asynchronous inter-process communication, and a Last-Writer-Wins (LWW) Conflict-Free Replicated Data Type (CRDT) algorithm to ensure mathematical convergence and eventual consistency across all replicas."*

---

## 2. High-Level Architecture & The Dual-IPC Model

### Why Two Different IPC Mechanisms? (The Fan-In vs. Fan-Out Model)

A common architectural question is: *"If you have Shared Memory, why do you need Message Queues?"*

```
                     FAN-IN (Collection)
   User 1 saves ─────► ┐
   User 2 saves ─────► ┼──► [ SINGLE SHARED GLOBAL BUFFER ] (/dev/shm)
   User 3 saves ─────► ┘    (Accumulates until count == 5)
                                      │
                                      │  Threshold reached (count >= 5)
                                      │  CAS leader drains & resets buffer
                                      ▼
                     FAN-OUT (Distribution)
                       ┌──────────────┼──────────────┐
                       ▼              ▼              ▼
                 /queue_user_1  /queue_user_2  /queue_user_3 (/dev/mqueue)
                 (Private MQ)   (Private MQ)   (Private MQ)
                       │              │              │
                       ▼              ▼              ▼
                     User 1         User 2         User 3
                   merges at      merges at      merges at
                   own pace       own pace       own pace
```

1. **Shared Memory (`/synctext_global_buffer`) = Fan-In**:
   - Collects and aggregates edits from all users into a shared batch until $N=5$ updates accumulate.
   - Avoids flooding the network/system on every single keystroke.
2. **POSIX Message Queues (`/queue_<uid>`) = Fan-Out**:
   - Once 5 edits accumulate, the global buffer **must be wiped immediately** (`count = 0`) to accept the next batch.
   - If users read directly from shared memory, slower users would miss edits when faster users clear the buffer!
   - Copying the batch into each user's private kernel-managed message queue decouples processes: every user receives their own private copy and merges at their own pace.

---

## 3. Deep Operating Systems & Memory Architecture

### A. Virtual Address Space vs. Physical RAM
* Operating systems isolate processes so Process 1 cannot read Process 2's memory.
* Addresses in your program (e.g., `0x7ffee400`) are **virtual addresses**, translated to physical RAM by the CPU's **MMU (Memory Management Unit)** using **Page Tables**.

### B. What `mmap()` with `MAP_SHARED` Actually Does
```cpp
void *addr = mmap(nullptr, sizeof(SharedRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```
1. **Finds Virtual Address Space**: Finds an unused virtual address range in the process's page table.
2. **Creates Page Table Entries (PTE)**: Maps those virtual addresses directly to the **physical RAM frames** allocated for `/synctext_registry4`.
3. **`MAP_SHARED`**: Tells the kernel that writes made to this virtual address must update the underlying physical RAM frame directly, making changes immediately visible to all other processes mapping the same object.
4. **Why No Segfault?**: A Segmentation Fault (`SIGSEGV`) occurs when the MMU encounters an unmapped page or a permissions violation. Because `mmap` registers a valid PTE with `PROT_READ | PROT_WRITE`, the MMU permits the access without error.

### C. Does Shared Memory Live in the Heap?
**NO.** In the Linux process memory map, it lives in the **Memory Mapping Segment** (between the heap and the stack):

```
High Addresses (0x7FFFFFFFFFFF)
 ├──────────────────────────────────────────┤
 │ STACK (grows downward ▼)                 │  <-- Local variables
 ├──────────────────────────────────────────┤
 │ MEMORY MAPPING SEGMENT (mmap region)     │  <-- ⭐ SHARED MEMORY LIVES HERE!
 │ (/dev/shm, libc.so, large mallocs)       │
 ├──────────────────────────────────────────┤
 │ HEAP (grows upward ▲ via brk/sbrk)       │  <-- Standard malloc / new (< 128KB)
 ├──────────────────────────────────────────┤
 │ BSS & Data Segments                      │  <-- Global/static variables
 ├──────────────────────────────────────────┤
 │ Text Segment (.text)                     │  <-- Binary code (Read-only)
Low Addresses (0x000000000000)
```
* **Heap vs. `mmap`**: The heap is managed by user-space allocators (`ptmalloc`) via `brk()`. `mmap()` is a kernel system call. Calling `free()` on an `mmap` pointer will corrupt memory or crash; you **must** use `munmap()`.
* **Does `malloc` use `mmap`?**: Yes, for allocations $\ge 128\text{ KB}$ (`MMAP_THRESHOLD`), `malloc()` calls `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` to avoid heap fragmentation, but it steps *outside* the heap to do so.

### D. `munmap()` vs. `shm_unlink()`
* **`munmap(addr, size)`**: Deletes the entries from **this process's** Page Table and flushes the CPU's TLB (Translation Lookaside Buffer). It does **not** delete the shared memory from RAM.
* **`shm_unlink(SHM_NAME)`**: Removes the shared memory object from `/dev/shm`. The kernel reclaims the physical RAM once all processes have closed and unmapped it.

---

## 4. Lock-Free Programming & Atomic Primitives

### A. Is `atomic` a Datatype?
**No.** `std::atomic<T>` is a **C++ template wrapper class** (`#include <atomic>`).
* Wrapping a primitive type (like `std::atomic<bool>` or `std::atomic<int>`) tells the compiler to replace standard load/store instructions with **hardware-level atomic assembly instructions** (e.g., `LOCK CMPXCHG`, `LOCK XADD` on x86).
* It enforces CPU cache coherency across cores and prevents compiler register-caching.

### B. Why Atomics Over Mutexes?
1. **Crash Deadlock Immunity**: If a process crashes while holding an inter-process mutex, the entire system deadlocks permanently. Atomics have no critical section to hold; a process crash cannot deadlock peers.
2. **Performance**: Executed directly in CPU hardware cache in nanoseconds without kernel `futex` context switching.

### C. `compare_exchange_strong` vs. `compare_exchange_weak`
* **`compare_exchange_weak`**: Can experience **spurious failures** (returning `false` even if the value matched `expected`). It is common on Load-Linked/Store-Conditional (LL/SC) architectures like ARM and is used inside tight `while` loops.
* **`compare_exchange_strong`**: Guarantees no spurious failures. We used `strong` in SyncText because our registration loop checks each array slot once sequentially (`for i = 0 to 4`); a spurious failure would cause a slot to be falsely skipped.

### D. The 4 Atomic Implementations in SyncText:

| Primitive Used | Location | Purpose | How It Works |
| :--- | :--- | :--- | :--- |
| **`compare_exchange_strong`** | `registry.cpp:59` | User slot reservation | Checks if `active == false`. If true, atomically sets `active = true`. Exactly one process wins. |
| **`fetch_add(1)`** | `global_broadcast.cpp:78` | Staging edits in buffer | Atomically increments `count` and returns the old index. Avoids `count++` race conditions. |
| **`compare_exchange_strong`** | `global_broadcast.cpp:109`| Broadcast leader election | Flips `broadcast_in_progress` from `false` to `true`. Ensures only one process drains the buffer. |
| **Memory Fences (`acquire`/`release`)** | `global_broadcast.cpp:90,114`| Memory visibility | Prevents CPU instruction reordering. Ensures text `memcpy` finishes before `count` update is visible. |

---

## 5. Module-by-Module Code Walkthrough

### 5.1 [update.h](file:///d:/Projects/SyncText/src/update.h) (Data Contract)
```cpp
struct UpdateObject {
    enum class Type { Insert, Delete, Replace };
    Type op;
    int line, col_start, col_end;
    char old_text[256];
    char new_text[256];
    uint64_t timestamp_ms;
    char user_id[32];
};
```
* **Why `char[]` instead of `std::string`?**: `std::string` contains a heap pointer (`char*`). Passing pointers across processes causes a Segmentation Fault because virtual address spaces are isolated. Fixed-size arrays make `UpdateObject` **POD / Trivially Copyable**, allowing raw `memcpy` and direct message queue transmission.
* **Why `user_id`?**: Deterministic tiebreaker when two edits share the exact same microsecond timestamp.

---

### 5.2 [registry.cpp](file:///d:/Projects/SyncText/src/registry.cpp) (Lock-Free Peer Discovery)
* Shared Memory file: `/dev/shm/synctext_registry4`.
* **Cold-Start Check**:
  ```cpp
  struct stat shm_stat; fstat(fd, &shm_stat);
  bool is_new = (shm_stat.st_size == 0);
  ```
  If `is_new == true`, this is the first process to boot, so it runs `memset(reg, 0, ...)` to initialize clean memory. Subsequent processes skip this, preventing accidental data wipes.
* **Slot Acquisition**: Iterates 0 to 4 using `compare_exchange_strong(false, true)` on `entries[i].active`.
* **Deregistration & Memory Hygiene**:
  ```cpp
  reg->entries[i].active = false;
  memset(reg->entries[i].user_id, 0, sizeof(...));
  ```
  Marks slot free and zeroes memory to prevent stale user artifacts.
* **Least Privilege**: `list_users()` and `get_all_users()` open with `O_RDONLY` and `PROT_READ`.

---

### 5.3 [messaging.cpp](file:///d:/Projects/SyncText/src/messaging.cpp) (POSIX Message Queues)
* Queue naming: `/queue_<user_id>` (e.g., `/queue_user_1`).
* **Why Message Queues over Pipes/Sockets?**: Preserves message boundaries (1 `mq_send` = 1 complete `UpdateObject`). No framing, delimiters, or connection handshake needed.
* **Stale Queue Cleanup**: Calls `mq_unlink(mq_name.c_str())` before `mq_open` to flush old queues left behind by crashed runs.
* **Polling vs. Blocking Trade-off**:
  - Configured with `O_NONBLOCK` + `this_thread::sleep_for(100ms)`.
  - *Trade-off*: Allows the thread to wake up periodically and check an exit flag for graceful shutdown. However, it introduces 100ms polling latency and minor CPU wakeups.
  - *Production Alternative*: A blocking `mq_receive()` has **zero CPU overhead** and **zero latency** (instant wakeup). To shut down cleanly, the main thread can send a dummy `SHUTDOWN` message to its own queue upon exit.
* **The `epoll` Discussion**:
  - For a thread watching a single queue, blocking `mq_receive()` is ideal.
  - However, in a full production reactor architecture, an **`epoll` event loop** could multiplex:
    1. A Linux `inotify` descriptor for file saves,
    2. A POSIX Message Queue descriptor (`mqd_t`) or `mq_notify` for peer updates,
    3. A `signalfd` descriptor for OS signals (`SIGINT`).
    This would unite file I/O and messaging into a single non-blocking event-driven loop with 0% idle CPU usage.

---

### 5.4 [crdt_merge.h](file:///d:/Projects/SyncText/src/crdt_merge.h) (CRDT Merge Engine)
Resolves edits using range-level Last-Writer-Wins (LWW) in 3 steps:

1. **Bucket Updates by Line**: `std::map<int, std::vector<UpdateObject>> line_updates;`
2. **Greedy Priority Selection**:
   - Sort candidates by LWW priority: `timestamp_ms` descending, tiebreaker `user_id` ascending.
   - Accept candidate 0 (winner). For remaining candidates: check overlap with accepted winners using interval math:
     $$\max(s_1, s_2) < \min(e_1, e_2)$$
   - If it overlaps, discard (it's an older conflicting edit). If no overlap, accept as another winner!
3. **Right-to-Left In-Place Replacement (The Index Shift Solution)**:
   - Sort winning edits in **descending column order** (`col_start > b.col_start`).
   - Apply edits from right to left using `cur_line.replace(...)`.
   - **Why?** Modifying text at the end of a line never alters or corrupts the column indices of text earlier in the line!

---

### 5.5 [global_broadcast.cpp](file:///d:/Projects/SyncText/src/global_broadcast.cpp) (Batching & Fan-Out)
* Shared Memory file: `/dev/shm/synctext_global_buffer`.
* **Adding Updates**: `current_count = g_buffer->count.fetch_add(1)`. Safely reserves array slot.
* **Threshold Detection**: Background thread checks `count >= 5` every 500ms.
* **Single Winner Broadcast**:
  ```cpp
  bool expected = false;
  if (!g_buffer->broadcast_in_progress.compare_exchange_strong(expected, true)) return;
  ```
  Only one thread wins the right to drain the buffer and call `broadcast_merged_updates()`.

---

### 5.6 [file_monitor.cpp](file:///d:/Projects/SyncText/src/file_monitor.cpp) (Local Loop & Diffing)
* **Polling**: Heartbeat runs every 400ms checking `stat().st_mtime`.
* **Prefix / Suffix Diffing (`find_diff`)**:
  - Matches characters from left (`start++`) and right (`end--`).
  - Extracts modified column range $[col\_start, col\_end]$ and sets operation type (`Insert`, `Delete`, `Replace`).

---

### 5.7 [main.cpp](file:///d:/Projects/SyncText/src/main.cpp) (Thread Orchestration)
Runs **3 concurrent threads** per process:
1. **Thread 1 (Main Thread)**: Runs `FileMonitor::run()`. Polls file, runs CRDT merge, renders ANSI terminal UI.
2. **Thread 2 (Listener Thread)**: Detached thread running `listen_for_updates()`. Blocks on message queue and pushes to `g_received_updates`.
3. **Thread 3 (Global Monitor Thread)**: Detached thread running `GlobalBroadcast::monitor_global_buffer()`. Watches for 5-update threshold.
* **Signal Handling**: `signal(SIGINT, on_exit)` intercepts `Ctrl+C` and cleanly calls `Registry::unregister_user()`.

---

## 6. The "Infinite Echo Loop" & Its Solution

**The Trap**: User 1 edits file $\rightarrow$ User 2 receives update, merges, and writes to disk $\rightarrow$ Disk modification time (`mtime`) on User 2 updates $\rightarrow$ User 2 thinks its local user typed $\rightarrow$ User 2 broadcasts back to User 1 $\rightarrow$ **Infinite Ping-Pong Loop!**

```
                           HOW SYNCTEXT BREAKS THE LOOP:
                                         │
                                         ▼
            Step 1: attempt_merge() runs on User 2
                    CRDT::apply_updates(m_lines)
                                         │
                                         ▼
            Step 2: write_back_file()
                    Writes to user_2_doc.txt (OS updates disk mtime)
                                         │
                                         ▼
            Step 3: 💡 THE NEUTRALIZER
                    m_last_mtime = get_mtime(m_filename);
                    (Cached mtime updated to match disk immediately!)
                                         │
                                         ▼
            Step 4: Heartbeat check runs next:
                    time_t m = get_mtime(m_filename);
                    if (m != m_last_mtime)  <── FALSE! Timestamps match!
                                         │
                                         ▼
            Step 5: detect_changes() is SKIPPED. Zero echo broadcast!
```

**Second Layer of Defense**: Even if clocks drifted, `m_lines` was updated during merge. Diffing reads the file, finds `old_line == new_line`, and generates zero updates!

---

## 7. Comprehensive Questions & Answers

#### Q1: "Why choose CRDT over Operational Transformation (OT)?"
> **Answer**: OT requires a centralized sequencer to transform concurrent operations (like Google Docs historically did). SyncText is fully decentralized; each node must merge out-of-order edits independently. CRDT guarantees mathematical convergence (commutativity, associativity, idempotency) without a coordinator.

#### Q2: "How are ties broken if two edits have the exact same timestamp?"
> **Answer**: Author user IDs are compared alphabetically (`strcmp(a.user_id, b.user_id) < 0`). Because alphabetical ordering is identical on all machines, every node independently selects the exact same winner.

#### Q3: "What is the consequence of Last-Writer-Wins (LWW)?"
> **Answer**: Simplicity and deterministic convergence. The trade-off is lost intent: if two users edit the exact same column range concurrently, the later edit completely overwrites the earlier one rather than interleaving characters.

#### Q4: "Why link with `-lpthread` and `-lrt`?"
> **Answer**: `-lpthread` links POSIX multi-threading support (`std::thread`). `-lrt` links POSIX real-time extensions, which are required on Linux for shared memory (`shm_open`) and message queues (`mq_open`).

#### Q5: "How is lock-free synchronization guaranteed across processes?"
> **Answer**: By using `std::atomic<bool>` and `std::atomic<int>` in POSIX Shared Memory, utilizing `compare_exchange_strong` for slot reservations and leader election, `fetch_add` for index reservations, and `acquire-release` memory fences to ensure CPU cache coherency.

#### Q6: "Why use `compare_exchange_strong` instead of `compare_exchange_weak`?"
> **Answer**: `compare_exchange_weak` can fail spuriously on LL/SC architectures (like ARM) and is intended for tight `while` loops. Our slot-acquisition loop checks each array slot once sequentially (`for i = 0 to 4`); using `strong` guarantees that a free slot is never falsely skipped.

#### Q7: "Why is `mq_receive` non-blocking with a 100ms sleep? Wouldn't blocking be better?"
> **Answer**: A non-blocking poll with sleep was chosen so the thread can periodically wake up to check thread termination flags for clean shutdown. However, for maximum efficiency, a blocking `mq_receive()` is superior because it eliminates polling latency and reduces idle CPU usage to zero. In production, clean shutdown with a blocking queue is accomplished by sending a self-targeted shutdown message upon exit.

#### Q8: "Could `epoll` be used instead of multi-threading?"
> **Answer**: Yes. In a production reactor design, `epoll` could multiplex a Linux `inotify` file descriptor, POSIX message queue descriptors, and `signalfd` on a single thread. For SyncText's scope, a dedicated thread per task (file monitoring, message listening, and global monitoring) provides clean separation of concerns.

#### Q9: "What happens if a user process crashes or receives `kill -9` without a clean exit?"
> **Answer**: Because registration uses atomic flags without mutexes, a crash will never deadlock the remaining processes. However, `kill -9` prevents signal handlers from running, leaving the slot marked `active = true` (a ghost user). In production, this is solved by having each user write a heartbeat timestamp (`atomic<uint64_t> last_seen`) or checking process IDs using `kill(pid, 0)`.

#### Q10: "What happens if physical system clocks drift across different machines?"
> **Answer**: Physical clock drift could cause a machine with a fast clock to systematically win Last-Writer-Wins conflicts. In a multi-machine network, physical timestamps would be augmented or replaced with **Lamport Logical Timestamps** or **Vector Clocks** to track causal precedence independently of physical hardware clocks.

#### Q11: "What are the limitations of a fixed 256-byte text buffer and how can it be scaled?"
> **Answer**: Using a fixed-size `char new_text[256]` ensures `UpdateObject` is trivially copyable for raw IPC memory copying, but it truncates edits longer than 255 characters. To scale to arbitrary lengths, we would use chunking/paging or variable-length Type-Length-Value (TLV) packets.

---

## 8. Compilation, Execution & Testing Guide

### A. Compilation Command
```bash
g++ -std=c++17 src/*.cpp -lpthread -lrt -o editor
```
* `-std=c++17`: Enables C++17 language features.
* `-lpthread`: Links POSIX threads (`std::thread`).
* `-lrt`: Links POSIX real-time extensions (required for `shm_open`, `mq_open`).

---

### B. Running Multi-User Simulation

Open **3 separate Linux terminal windows**:

**Terminal 1 (User 1):**
```bash
./editor user_1
```

**Terminal 2 (User 2):**
```bash
./editor user_2
```

**Terminal 3 (User 3):**
```bash
./editor user_3
```

---

### C. Testing Scenarios

#### Scenario 1: Basic Synchronization (Non-Conflicting Edits)
1. Open `user_1_doc.txt` in any text editor (`nano`, `vim`, or `gedit`).
2. Change Line 0 from `"int x = 10;"` to `"int x = 50;"` and save.
3. Open `user_2_doc.txt` and change Line 1 from `"int y = 20;"` to `"int y = 100;"` and save.
4. Make 3 more edits across any files until the global buffer reaches **5 total updates**.
5. **Expected Result**: All 3 terminal displays update automatically and display both `x = 50` and `y = 100`.

#### Scenario 2: Concurrent Conflicting Edits (LWW Verification)
1. User 1 edits Line 0 to `"int x = 75;"` and saves.
2. User 2 almost simultaneously edits Line 0 to `"int x = 99;"` and saves.
3. Once 5 total updates accumulate, the Global Broadcast module compares timestamps on Line 0, columns 8–10.
4. **Expected Result**: The edit with the later timestamp wins. All 3 terminals converge to `"int x = 99;"`.
