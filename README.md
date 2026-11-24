# NovaPM - Process and Resource Monitoring Manager

C++20 desktop monitor for **Linux (/proc)** and **Windows (Win32)**: live process table, process control,
SQLite history, command-line tool, and a **Threshold Guard** that alerts at 60 % and suspends at 80 %.

## Features
| Area | What it does |
|------|--------------|
| Process table | PID, name, user, state, CPU %, memory, threads, PPID, **container**; sort, filter, select |
| **Details tab** | executable, command line (Linux), start time, priority/nice, open files/handles, parent, children, container, guard status, **per-process CPU + memory history graphs** from the database |
| Process tree | parent/child tree with the same actions |
| Control | Terminate / Stop (suspend) / Continue / Kill, with permission and exited-PID handling |
| System tab | CPU, RAM, swap, uptime, process and thread counts, live graphs |
| SQLite history | snapshots, system stats, events, alerts; read-only SQL console; 24 h retention |
| **Threshold Guard** | alert at >= 60 %, suspend at >= 80 % (3 samples in a row); CPU and memory; protected system processes; resume grace; alert-only mode |
| **CLI** `novapm-cli` | `top`, `watch` (headless guard), `history`, `alerts`, `query`, `signal`, `containers`, `config` |
| **Containers** | Linux: Docker / containerd / CRI-O / Podman container id per process (from cgroups) |
| **Settings file** | all thresholds and intervals persisted; "Save settings" button, `novapm-cli config set` |
| Packaging | CPack: `.tar.gz` + `.deb` (Linux), `.zip` (Windows); GitHub Actions CI |

## Build
Needs CMake >= 3.16 and a C++20 compiler.

**Linux**
```
sudo apt install cmake g++ libsqlite3-dev xorg-dev libgl1-mesa-dev     # GUI needs X11 + OpenGL
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure         # unit tests + real-process end-to-end test
cd build && cpack                                   # novapm-1.0.0-Linux.tar.gz and novapm_1.0.0_amd64.deb
```
CLI only (no GUI, no OpenGL): `cmake -S . -B build -DNOVAPM_BUILD_GUI=OFF`

**Windows** (Visual Studio 2022 or MinGW): run `build.bat`, or
`cmake -S . -B build && cmake --build build --config Release`. SQLite, Dear ImGui and GLFW are downloaded by CMake.

Run `build/novapm` (GUI) or `build/novapm-cli help`. `novapm <command>` also works (e.g. `novapm watch`).
Process control of other users' processes needs `sudo` (Linux) or Administrator (Windows).

## Threshold Guard
Each sample, every process is compared with two thresholds (CPU % of one core, and memory % of RAM):

| Usage | Action |
|-------|--------|
| >= 60 % | warning banner, Alerts tab entry, yellow row, logged. Process untouched |
| >= 80 % for 3 samples | process suspended (SIGSTOP / NtSuspendProcess), red then blue row, logged. **Resume** button |

Safeguards: sustain count, alert cooldown + re-arm hysteresis, never stops PID <= 4 / system processes / NovaPM itself,
60 s grace after Resume, `--no-stop` / "Auto-stop" off for alert-only mode, failure reporting.
Everything is stored in the `alerts` and `events` tables:
`SELECT name, action, COUNT(*) FROM alerts GROUP BY name, action;`

## Command line
```
novapm-cli top [--n 15] [--mem]                  sample twice, show the busiest processes
novapm-cli watch --warn 60 --stop 80 [--no-stop] headless monitor + guard (Ctrl+C to end, --duration N seconds)
novapm-cli history PID [--minutes 60]            stored CPU / memory samples with a text graph
novapm-cli alerts [--limit 20]                   recent alerts and automatic stops
novapm-cli query "SELECT ..."                    read-only SQL (writes are rejected)
novapm-cli signal PID term|stop|cont|kill        send a signal, logged in events
novapm-cli containers                            processes grouped by container
novapm-cli config [set KEY VALUE]                show or change settings
```
Global options: `--db FILE`, `--config FILE`. Exit codes: 0 ok, 1 runtime error, 2 usage error.

## Settings file
`~/.config/novapm/novapm.ini` (Linux) or `%APPDATA%\NovaPM\novapm.ini` (Windows); override with `NOVAPM_CONFIG`.
See `packaging/novapm.ini.sample` for every key.

## Layout
```
src/threshold_guard.*   alert / stop policy (pure logic)         src/config.*          settings file
src/monitor_core.*      executes guard decisions, lifecycle log  src/cli.*             command-line interface
src/database.*          SQLite layer (+ per-process history)     src/main.cpp          Dear ImGui GUI
src/platform/linux/     /proc collector, system monitor, kill(2) src/platform/win32/   Win32 equivalents
tests/                  guard_test, core_test, e2e_linux.sh, load_gen
```

## Known limits
* Windows: command line of other processes is not shown; "Terminate" only closes windows (use Kill for console programs).
* Container ids are read from cgroups only; names/images are not resolved (no Docker API calls).
* The GUI is X11 (also runs under Wayland through XWayland).
* A threshold rule is not a diagnosis: a legitimately heavy job is also suspended at 80 % unless alert-only mode is on.
