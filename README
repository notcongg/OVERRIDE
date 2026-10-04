<div align="center">

# OVERRIDE

<p><strong>CONTROL THE SYSTEM. BREAK THE SYSTEM. UNDERSTAND THE SYSTEM.</strong></p>

<p>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/badge/CMake-build-064F8C?style=for-the-badge&logo=cmake&logoColor=white" alt="CMake">
  <img src="https://img.shields.io/badge/dependencies-stdlib%20only-lightgrey?style=for-the-badge" alt="stdlib only">
  <img src="https://img.shields.io/badge/tests-1611%20checks-success?style=for-the-badge" alt="1611 checks">
  <img src="https://img.shields.io/badge/CTest-4%2F4%20PASS-success?style=for-the-badge" alt="CTest">
  <img src="https://img.shields.io/badge/license-MIT-green?style=for-the-badge" alt="MIT License">
</p>

<p>
  Experimental <strong>system manipulation + simulation environment</strong>
  built with C++17, CMake, and the standard library.
</p>

<p>
  Nodes · Processes · Services · Network · VFS · Kernel · Time · Recovery · Events
</p>

</div>

> [!IMPORTANT]
> **OVERRIDE is fully simulated.** It does not control the real operating system,
> network, filesystem, or hardware.

---

## What is OVERRIDE?

OVERRIDE is a self-contained simulated system where you can intentionally
break, inspect, manipulate, recover, and observe a virtual machine-like world.

The simulation includes:

* deterministic nodes and dependencies
* simulated processes and services
* networking and resources
* a virtual filesystem
* kernel/substrate state
* accounts, permissions, and simulated `sudo`
* checkpoints, rewind, and persistent worlds
* recovery packages
* one unified event/causality ledger

**OVERSHELL** is the command-line interface over the engine.

---

## Quick Start

### Build

```sh
cmake -S . -B build -G "Unix Makefiles"
cmake --build build --parallel
```

### Test

```sh
ctest --test-dir build
```

### Run

```sh
./build/override
```

### Headless

```sh
./build/override --cmd "status; break server; trace client; repair server; status"
```

```sh
./build/override --script OVERSHELL/sandbox/cascading_failure.ovr
```

> [!NOTE]
> Use another CMake generator when your platform/toolchain requires one.

---

## Example

```text
[root@override]# status

client    ONLINE
server    ONLINE
database  ONLINE
cache     ONLINE

[root@override]# break server

server: ONLINE -> FAILED (#2)
client: ONLINE -> DEGRADED
        dependency server is FAILED

[root@override]# tick 10
advanced 10 tick(s) -> t=+10

[root@override]# trace client
...

[root@override]# repair server
server: FAILED -> ONLINE (#9)
```

> [!TIP]
> Failures are stateful. Breaking a node, corrupting the VFS, killing a process,
> or breaking a boot dependency can create real consequences inside the simulation.

---

## Core Systems

<div align="center">

| System          | Highlights                                        |
| --------------- | ------------------------------------------------- |
| **Nodes**       | state, health, dependencies, cascading failures   |
| **Processes**   | lifecycle, CPU/RAM, signals, threads, PIDs        |
| **Services**    | managed processes, policies, restart loops        |
| **Network**     | routing, latency, congestion, packet loss         |
| **VFS**         | virtual files, ownership, permissions, corruption |
| **Kernel**      | boot, modules, devices, syscalls, scheduler       |
| **Faults**      | stacked faults, severity, causal propagation      |
| **Time**        | tick, pause, checkpoints, restore, rewind         |
| **Worlds**      | persistent `.ord` worlds and atomic saves         |
| **Recovery**    | `sudo get/load`, transactional repair             |
| **Events**      | unified append-only ledger + causal tracing       |
| **Determinism** | seeded simulation + reproducible state            |

</div>

> [!IMPORTANT]
> OVERRIDE separates **simulation state** from the host system.
> The real machine stays outside the simulation boundary.

---

## Documentation

The README is intentionally short. Detailed behavior lives in [`documents/`](documents/).

| Document                                                 | Covers                              |
| -------------------------------------------------------- | ----------------------------------- |
| [`documents/ARCHITECTURE.md`](documents/ARCHITECTURE.md) | Engine and subsystem architecture   |
| [`documents/SYSTEMS.md`](documents/SYSTEMS.md)           | Simulation systems and interactions |
| [`documents/COMMANDS.md`](documents/COMMANDS.md)         | Full OVERSHELL command reference    |
| [`documents/VFS.md`](documents/VFS.md)                   | Virtual filesystem and permissions  |
| [`documents/RECOVERY.md`](documents/RECOVERY.md)         | Boot, recovery packages, rollback   |
| [`documents/WORLDS.md`](documents/WORLDS.md)             | Worlds, persistence, saves, rewind  |
| [`documents/TESTING.md`](documents/TESTING.md)           | Test architecture and determinism   |

> [!NOTE]
> Additional documentation can be added under `documents/` without expanding
> the main README.

---

## Project Layout

```text
OVERRIDE/
├── OVERSHELL/      shell frontend
├── KNRL/           simulation engine + virtual system
├── ORDC/           deterministic .ord document system
├── documents/      technical documentation
├── CMakeLists.txt
├── README.md
├── LICENSE
└── .gitignore
```

---

## Testing

Current validation:

```text
mvp_test ALL_TESTS_OK
checks=1611
failures=0

CTest
4/4 PASS
```

The test suite covers the engine, shell, persistence, VFS, permissions,
processes, services, networking, recovery, boot, determinism, and ORDC.

---

## Design Principles

```text
SIMULATION FIRST
STATE HAS CONSEQUENCES
EVERYTHING SHOULD BE EXPLAINABLE
DETERMINISM MATTERS
RECOVERY SHOULD ACTUALLY RECOVER
THE HOST STAYS UNTOUCHED
```

---

## License

<div align="center">

**MIT License**

Copyright (c) 2026 Congg

See [`LICENSE`](LICENSE) for the full license text.

</div>

---

<div align="center">

<sub>Experimental software for system simulation, failure analysis, recovery, and controlled chaos.</sub>

<br><br>

<strong>CONTROL THE SYSTEM. BREAK THE SYSTEM. UNDERSTAND THE SYSTEM.</strong>

</div>
