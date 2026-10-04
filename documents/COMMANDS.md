# OVERRIDE Systems

> Overview of the major subsystems inside OVERRIDE.

## 1. Kernel

The kernel represents the core runtime state of the simulated system.

It is responsible for:

* Boot state
* Kernel image
* System initialization
* Kernel integrity
* Kernel restoration

The kernel is independent from the host OS kernel.

```text
Kernel
├── Image
├── Boot state
├── Initialization
└── Integrity
```

---

## 2. Root Filesystem

The rootfs represents the baseline filesystem required for the simulated OS to operate.

It contains system files and directories required by the runtime.

```text
/
├── boot/
├── bin/
├── etc/
├── home/
├── tmp/
└── ...
```

Rootfs recovery restores the known-good baseline while preserving user data outside the protected baseline.

---

## 3. Virtual Filesystem

The VFS provides filesystem semantics inside OVERRIDE.

It handles:

* Files
* Directories
* Paths
* Creation
* Deletion
* Reading
* Writing
* Permissions
* Filesystem integrity

The VFS is simulated and does not represent the host filesystem directly.

---

## 4. Worlds

A world is an isolated simulated system instance.

Each world owns its own logical state.

```text
World
├── Kernel
├── Rootfs
├── VFS
├── Processes
├── Services
├── Users
├── Clock
└── Events / Ledger
```

Changing one world must not silently modify another.

---

## 5. Processes

Processes represent running programs inside the simulated environment.

A process may contain:

* PID
* Name
* State
* Owner
* Runtime information

Typical process states include:

```text
NEW
RUNNING
STOPPED
TERMINATED
```

The process system is simulation logic rather than native OS process management.

---

## 6. Services

Services represent long-running system components.

A service can be:

```text
installed
enabled
running
stopped
disabled
```

Services are managed by the simulated environment and do not automatically become Windows services.

---

## 7. Authentication

Authentication controls access to privileged operations.

The normal user operates as:

```text
conggowner
```

Root operations require elevated privileges.

Two execution forms are supported:

```text
sudo <command>
```

and:

```text
sudo -a
```

The first is temporary; the second activates persistent root mode for the current session.

---

## 8. Time

OVERRIDE maintains its own simulated clock.

The clock can represent system time independently of the host clock.

Example world configuration:

```text
[clock]
paused = false
```

Time belongs to the world state and therefore must remain isolated between worlds.

---

## 9. Event System

The event system records important state changes.

Examples include:

```text
KERNEL_RESTORED
VFS_RESTORED
PACKAGE_REJECTED
USER_PACKAGE
FAULT_RESOLVED
```

Events are useful for:

* Auditing
* Debugging
* Recovery history
* Testing
* Observing system behavior

---

## 10. Recovery

The recovery subsystem provides controlled restoration of damaged system state.

Supported recovery targets:

```text
kernel
rootfs
```

The workflow is:

```text
get
 │
 ▼
Recovery Package
 │
 ▼
load
 │
 ▼
Integrity Check
 │
 ▼
Transactional Restore
 │
 ├── success → Commit
 │
 └── failure → Rollback
```

Recovery requires root privileges.

---

## 11. Persistence

Persistence stores the logical state required to recreate a world after restarting OVERRIDE.

Persistent data includes system state such as:

* World configuration
* VFS state
* Kernel state
* Rootfs state
* Users
* Services
* Events

Persistence must not leak state between isolated worlds.

---

## 12. Ledger

The ledger provides persistent historical information about important system operations.

It is useful for tracking:

* State changes
* Administrative operations
* Recovery actions
* System events

The ledger is not intended to replace the current system state.

---

## 13. Command System

The command system connects user input to internal subsystems.

```text
Input
  │
  ▼
Parser
  │
  ▼
Dispatcher
  │
  ├── Kernel
  ├── VFS
  ├── Process
  ├── Service
  ├── Auth
  ├── Recovery
  └── World
```

Aliases should resolve into existing command implementations instead of duplicating behavior.

For example:

```text
crt
```

may resolve to:

```text
create
```

Similarly, privileged recovery commands share the same underlying `get` / `load` implementation.

---

## 14. ORDC

ORDC is the package/data format used by OVERRIDE for system recovery artifacts.

Recovery packages contain integrity metadata used to detect invalid or modified packages.

The recovery layer verifies package integrity before applying changes.

---

## 15. Subsystem Rules

Subsystems should follow these rules:

* Do not directly modify another subsystem's internal state.
* Use defined interfaces for cross-system operations.
* Keep host-side operations isolated from simulated operations.
* Validate privileged operations before mutation.
* Prefer transactional updates for destructive or recoverable operations.
* Emit events for significant state transitions.
* Keep behavior deterministic where possible.
