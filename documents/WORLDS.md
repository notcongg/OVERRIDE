# OVERRIDE Worlds

> Independent simulated system environments.

## 1. Overview

A world is an isolated instance of the OVERRIDE environment.

Each world maintains its own logical system state.

```text
World
├── Clock
├── Kernel
├── Rootfs
├── VFS
├── Processes
├── Services
├── Users
└── Events / Ledger
```

---

## 2. Isolation

Worlds are isolated from one another.

```text
┌──────────────┐
│   World A    │
│              │
│ Kernel       │
│ VFS          │
│ Processes    │
└──────────────┘

┌──────────────┐
│   World B    │
│              │
│ Kernel       │
│ VFS          │
│ Processes    │
└──────────────┘
```

Changing World A must not silently change World B.

---

## 3. World State

World state includes the components required to reconstruct the simulated environment.

Examples:

* Filesystem state
* Kernel state
* Rootfs state
* Users
* Services
* Processes
* Clock state
* Events
* Configuration

---

## 4. Clock

Each world owns its simulated clock.

Example configuration:

```ini
[clock]
paused = false
```

Clock state is persisted with the world.

---

## 5. Lifecycle

A world generally follows:

```text
Create
  │
  ▼
Initialize
  │
  ▼
Run
  │
  ▼
Persist
  │
  ▼
Reload
```

Reloading a world should reconstruct its previous logical state.

---

## 6. Recovery

Recovery operations are scoped to the active world.

```text
Active World
     │
     ▼
Recovery
     │
     ▼
World State
```

Recovery must not affect unrelated worlds.

---

## 7. World Configuration

World configuration describes how the simulated environment starts.

Example:

```ini
[clock]
paused = false
```

Configuration corruption may be detected and resolved by recovery when applicable.

---

## 8. Design Rules

* Keep worlds independent.
* Persist world-owned state explicitly.
* Do not use host-global state as world state.
* Scope recovery to the active world.
* Ensure world reloads are deterministic.
