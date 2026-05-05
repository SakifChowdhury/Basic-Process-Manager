# ⚡ Basic Process Manager: A Deep Dive into Systems Programming

Welcome! I built this **terminal-based Process Manager** from scratch in **C** to showcase my understanding of Operating Systems and Low-Level Systems Programming. 

Instead of relying on high-level wrappers, this project goes straight to the kernel level—parsing the Linux `/proc` virtual filesystem to compute live CPU deltas, read memory footprints, and track process states. It heavily leverages **POSIX system calls** (`fork`, `exec`, `kill`, `waitpid`, `setpriority`) and utilizes **Pthreads with Mutex locks** to safely manage a live background UI refresh cycle. 

If you're looking for clean, performant, and thread-safe C code that interacts directly with the Linux kernel, you're in the right place! 🚀

## Features
- **Real-Time Monitoring**: View live metrics such as CPU usage, Memory (RSS/VIRT), Process State, and Scheduling Priority.
- **Process Control**: Send POSIX signals (`SIGTERM`, `SIGKILL`, `SIGSTOP`, `SIGCONT`) and adjust scheduling priority (`renice`).
- **Process Tree**: Visualize parent-child process relationships with an intuitive tree view.
- **Search & Filter**: Quickly find processes by name or owner.
- **Thread-Safe**: Uses a background thread with mutex synchronization for smooth UI updates without blocking user input.

## Build and Run
This project requires a Linux environment (or WSL) and GCC.

```bash
make
./process_manager
```

## Demo

<p align="center">
  <a href="https://www.youtube.com/embed/4A-yltOADQU">
    <img src="https://img.youtube.com/vi/4A-yltOADQU/hqdefault.jpg" width="600" height="300" alt="Watch the video" />
  </a>
</p>

<p align="center">
  <video src="Project_Demo.mp4" controls="controls" width="100%"></video>
</p>
