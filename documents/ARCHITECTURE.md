# OVERRIDE Architecture

> Internal architecture and execution model of OVERRIDE.

## 1. Overview

OVERRIDE is a C++17-based simulated operating environment.

It models an isolated system containing:

* Kernel
* Root filesystem
* Virtual filesystem (VFS)
* Worlds
* Processes
* Services
* Authentication
* Time
* Recovery
* Event logging
* CLI / shell

The host operating system is never treated as the simulated system.

---

## 2. High-Level Architecture

```text
                         ┌──────────────────┐
                         │    OVERSHELL     │
                         │  CLI / Parser    │
                         └────────┬─────────┘
                                  │
                                  ▼
                         ┌──────────────────┐
                         │    Dispatcher    │
                         └────────┬─────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              ▼                   ▼                   ▼
        ┌───────────┐       ┌───────────┐       ┌───────────┐
        │   Auth    │       │   Kernel  │       │    VFS    │
        └───────────┘       └───────────┘       └───────────┘
              │                   │                   │
              └───────────────────┼───────────────────┘
                                  ▼
                         ┌──────────────────┐
                         │      World       │
                         │  State / Ledger  │
                         └────────┬─────────┘
                                  │
                                  ▼
                         ┌──────────────────┐
                         │    Persistence   │
                         └──────────────────┘
```

---

## 3. Command Flow

A command generally follows this path:

```text
Input
  │
  ▼
Parser
  │
  ▼
Command validation
  │
  ▼
Permission / authentication
  │
  ▼
Dispatcher
  │
  ▼
System operation
  │
  ▼
World / VFS / kernel state
  │
  ▼
Persistence + events
```

Commands should not directly manipulate unrelated subsystems.

---

## 4. Privilege Model

OVERRIDE has two root execution modes.

### One-shot root

```text
sudo <command>
```

Authenticates the user, executes one privileged operation, then returns to the previous session state.

### Persistent root

```text
sudo -a
```

Activates root mode for the current session.

The prompt changes from:

```text
[conggowner@override]$
```

to:

```text
[root@override]#
```

Root mode can be exited with:

```text
sudo -e
```

---

## 5. Recovery Architecture

Recovery is deliberately separated from normal system mutation.

```text
Recovery Package
      │
      ▼
 Integrity Check
      │
      ├── invalid ──► PACKAGE_REJECTED
      │
      ▼
 Transaction
      │
      ▼
 Apply
      │
      ├── failure ──► Rollback
      │
      ▼
 Commit
      │
      ▼
 Event
```

Recovery packages are stored outside the simulated world:

```text
recovery/
├── kernel.ord
└── rootfs.ord
```

`get` creates or retrieves valid recovery packages.

`load` is the operation that mutates the simulated world.

---

## 6. State Isolation

OVERRIDE maintains a strict separation between:

```text
Host filesystem
        │
        X
        │
Simulated filesystem
        │
        ├── World state
        ├── Kernel state
        ├── Rootfs
        └── User data
```

Commands operate inside the OVERRIDE environment.

Tests use isolated application data locations so that host state is not modified.

---

## 7. Worlds

A world represents an independent simulated system state.

```text
World
├── clock
├── kernel
├── rootfs
├── VFS
├── processes
├── services
├── users
└── ledger/events
```

World state must remain isolated from other worlds.

---

## 8. Persistence

Persistent state is stored by the OVERRIDE runtime rather than relying on the host operating system's semantics.

Persistence is responsible for:

* World state
* VFS state
* Kernel state
* Rootfs state
* Configuration
* Event/ledger data
* Recovery metadata

Loading the same world again must reconstruct its previous logical state.

---

## 9. Events

Important system operations generate events.

Examples:

```text
KERNEL_RESTORED
VFS_RESTORED
PACKAGE_REJECTED
USER_PACKAGE
FAULT_RESOLVED
```

Events provide an audit trail without becoming the source of truth for every subsystem.

---

## 10. Design Principles

OVERRIDE follows several core principles:

1. **Isolation** — never confuse the simulated system with the host.
2. **Explicit mutation** — privileged operations must be intentional.
3. **Transactional recovery** — failed repairs must be reversible.
4. **Single implementation path** — aliases should reuse existing command logic.
5. **Deterministic behavior** — identical state should produce predictable results.
6. **Testability** — core behavior must be executable and verifiable without manual interaction.
7. **Minimal dependencies** — the core runtime relies on C++17 and the standard library where possible.

---

## 11. Related Documentation

* [`SYSTEMS.md`](SYSTEMS.md) — system components
* [`COMMANDS.md`](COMMANDS.md) — CLI commands
* [`RECOVERY.md`](RECOVERY.md) — recovery system
* [`PERSISTENCE.md`](PERSISTENCE.md) — persistent state
* [`TESTING.md`](TESTING.md) — test architecture
