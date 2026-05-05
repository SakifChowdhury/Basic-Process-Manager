# Basic Process Manager

A lightweight, terminal-based process manager for Linux, written in C. This project interfaces directly with the Linux `/proc` virtual filesystem to list, monitor, and control running processes in real-time. 

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

<video src="Project_Demo.mp4" controls="controls" width="100%"></video>
