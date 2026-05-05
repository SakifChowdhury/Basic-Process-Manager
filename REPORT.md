# Basic Process Manager — Design & Implementation Report

**Course:** Operating Systems Design (CSCI 323)
**Language:** C (C11, POSIX) | **Platform:** Linux

---

## 1. Overview

This project implements a terminal-based Basic Process Manager that interfaces directly
with the Linux OS to list, monitor, and control running processes. It reads live data
from the `/proc` virtual filesystem — the same source used by `top` and `ps` — and
presents it through an interactive menu-driven terminal UI.

---

## 2. Architecture & Design

### 2.1 Three-File Modular Design

| File | Responsibility |
|---|---|
| `process_table.h` | Data structures, constants, function prototypes |
| `process_table.c` | /proc parsing, CPU calculation, mutex, sorting |
| `process_manager.c` | Terminal UI, signal dispatch, fork/exec demo, main loop |

### 2.2 Internal Process Table (Simulated PCB)

A fixed-size array of `ProcessEntry` structs (max 1024) acts as an in-memory process
table, directly analogous to the OS's own Process Control Block (PCB) table. Each
entry stores PID, PPID, name, user, state, priority, nice, threads, RSS, VSZ,
CPU percent, and a CPU snapshot for delta calculations.

### 2.3 CPU Usage Calculation

A delta method identical to `top` is used:

```
cpu% = (delta_process_ticks / delta_total_cpu_ticks) x 100 x num_cpus
```

Two consecutive calls to `pt_refresh()` capture tick counts from `/proc/stat`
(system-wide) and `/proc/[pid]/stat` (per-process). The difference gives an accurate
rolling CPU percentage over the refresh interval.

### 2.4 Mutex Synchronization

A `pthread_mutex_t` inside `ProcessTable` protects all reads and writes. The
background thread holds the lock only during the final `memcpy` swap. Display
functions use lock-copy-unlock to minimise contention.

### 2.5 Background Refresh Thread

A dedicated `pthread` calls `pt_refresh()` every 2 seconds. This demonstrates how
real process monitors achieve live updates without blocking user input.

---

## 3. OS Concepts Demonstrated

### 3.1 Process States (decoded from /proc/[pid]/stat field 3)
- `R` Running, `S` Sleeping, `D` Disk wait, `Z` Zombie, `T` Stopped

### 3.2 Signals & Process Control
The `kill(2)` system call is used to send:
- **SIGTERM** — graceful termination (catchable)
- **SIGKILL** — immediate forced kill (uncatchable)
- **SIGSTOP** — suspend execution (uncatchable)
- **SIGCONT** — resume a stopped process

### 3.3 Scheduling & Renice
`setpriority(PRIO_PROCESS, pid, nice)` adjusts scheduling priority.
Nice range: -20 (highest) to +19 (lowest). Only root may set negative values.

### 3.4 Memory Management
- **RSS (Resident Set Size):** Physical RAM pages currently held by the process
- **VSZ (Virtual Size):** Total virtual address space including unmapped regions

### 3.5 System Calls Used

All five system calls required by the project specification are explicitly
demonstrated in the Spawn Demo (option 7) and Wait/Reap Demo (option 8):

| System Call | Where Used | Purpose |
|---|---|---|
| `fork()` | `menu_spawn_demo()` | Creates a child process (duplicates the current process) |
| `execlp()` | `menu_spawn_demo()` – child branch | Replaces child's memory image with `sleep 30` |
| `kill(2)` | `send_signal_to_pid()` | Sends SIGTERM / SIGKILL / SIGSTOP / SIGCONT |
| `getpid()` | `menu_spawn_demo()` | Retrieves and displays this process's own PID |
| `waitpid()` | `menu_wait_demo()` | Reaps the demo child; offered in WNOHANG (poll) and blocking modes |
| `setpriority(2)` | `menu_renice()` | Adjusts scheduling priority (renice) |
| `pthread_create` | `main()` | Spawns background refresh thread |
| `pthread_mutex_lock/unlock` | Throughout | Mutual exclusion on shared process table |
| `clock_gettime(CLOCK_BOOTTIME)` | `read_proc_stat()` | Computes process start time |
| `sysconf(_SC_NPROCESSORS_ONLN)` | `read_proc_stat()` | Scales CPU% across cores |

**Demonstrating `getpid()` and `wait()`:**  
When the user selects option `[7] Spawn demo`, the application prints its own
PID via `getpid()`, then calls `fork()` and `execlp()`.  The child's PID is
stored globally.  Option `[8] Wait/reap demo child` then calls `waitpid()`,
allowing the user to choose between a non-blocking poll (`WNOHANG`) or a
full blocking wait — directly mirroring the POSIX `wait()` semantics covered
in class.

### 3.6 Parent-Child Tree
A recursive DFS starting from PID 1 (`init`/`systemd`) uses `ppid` fields to
render the full process hierarchy, demonstrating UNIX's hierarchical process model.

---

## 4. Concurrency & Synchronisation

The design applies the **producer-consumer pattern**:
- **Producer:** Background refresh thread writes updated data to the table
- **Consumer:** Main/UI thread reads the table for display
- **Primitive:** `pthread_mutex_t` guards all table access

The mutex is intentionally NOT held during the slow `/proc` scan — only during
the final atomic swap — to avoid starving the UI thread.

---

## 5. Build & Run

```bash
make          
./process_manager

make run
```

**Requirements:** GCC, GNU Make, Linux kernel >= 2.6 (/proc filesystem)

---

## 6. Conclusion

This project bridges theoretical OS concepts and a working system tool. By reading
from `/proc`, computing CPU deltas, dispatching POSIX signals, and synchronising a
shared data structure with mutexes, every major OS topic is covered: process
lifecycle, scheduling, memory management, IPC via signals, and concurrency.
