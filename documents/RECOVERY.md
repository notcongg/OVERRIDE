# OVERRIDE Recovery

> Recovery architecture, packages, integrity validation, and transactional restoration.

## 1. Overview

OVERRIDE provides recovery mechanisms for restoring damaged system state.

Supported targets:

```text
kernel
rootfs
```

Recovery is intentionally separated into two operations:

```text
get → prepare recovery package
load → apply recovery package
```

`get` does not modify the simulated world.

`load` is the only operation that applies the recovered state.

---

## 2. Recovery Workflow

```text
                 ┌──────────────┐
                 │    sudo get  │
                 └──────┬───────┘
                        │
                        ▼
                ┌────────────────┐
                │ Recovery .ord  │
                └───────┬────────┘
                        │
                        ▼
                ┌────────────────┐
                │ Integrity Check │
                └───────┬────────┘
                        │
                  valid │ invalid
                        │      └──────► PACKAGE_REJECTED
                        ▼
                ┌────────────────┐
                │     sudo load  │
                └───────┬────────┘
                        │
                        ▼
                ┌────────────────┐
                │   Transaction  │
                └───────┬────────┘
                        │
                 ┌──────┴──────┐
                 ▼             ▼
              Commit        Rollback
```

---

## 3. Recovery Packages

Packages are stored in the application's recovery directory:

```text
recovery/
├── kernel.ord
└── rootfs.ord
```

Packages contain the information required to restore a known-good system state.

Recovery metadata includes integrity information such as:

* Package version
* SHA-256
* VFS digest

---

## 4. `get`

Syntax:

```text
sudo get kernel
sudo get rootfs
sudo get kernel,rootfs
```

`get` performs package preparation.

### Guarantees

A valid existing package is reused.

`get` must not:

* Modify the current world
* Modify the world ledger
* Corrupt the active system
* Replace a valid package unnecessarily

The operation may create a package when no valid recovery package exists.

---

## 5. `load`

Syntax:

```text
sudo load kernel
sudo load rootfs
sudo load kernel,rootfs
```

`load` performs the actual restoration.

Before changing system state, the package is validated.

```text
Package
   │
   ▼
Parse
   │
   ▼
Validate metadata
   │
   ▼
Verify integrity
   │
   ▼
Begin transaction
```

Invalid packages are rejected before mutation.

---

## 6. Kernel Recovery

Kernel recovery restores the known-good kernel image.

Conceptually:

```text
Recovery Package
      │
      ▼
Validate
      │
      ▼
Backup current state
      │
      ▼
Restore /boot/kernel
      │
      ▼
Commit
```

Successful restoration produces a recovery event:

```text
KERNEL_RESTORED
```

---

## 7. Rootfs Recovery

Rootfs recovery restores the known-good baseline filesystem.

The operation:

1. Validates the package.
2. Starts a transaction.
3. Restores baseline system files.
4. Recreates required directories.
5. Preserves user files outside the baseline.
6. Resolves recoverable configuration corruption.
7. Commits the transaction.

A successful filesystem restoration produces:

```text
VFS_RESTORED
```

Configuration faults resolved during recovery may additionally produce:

```text
FAULT_RESOLVED
```

---

## 8. User Data Preservation

Recovery must not blindly replace the entire filesystem.

The rootfs operation distinguishes between:

```text
System baseline
User data
```

Conceptually:

```text
/
├── system files      ← recoverable
├── system directories ← recoverable
│
└── user data         ← preserved
```

This allows recovery without unnecessarily destroying user-created content.

---

## 9. Transactions

Recovery operations are transactional.

Before mutation, the system creates a rollback journal containing the state necessary to undo the operation.

```text
Begin
  │
  ├── Backup
  ├── Apply
  ├── Validate
  │
  ├── Success ──► Commit
  │
  └── Failure ──► Rollback
```

The system must never intentionally leave a partially applied recovery state.

---

## 10. Rollback

If any recovery step fails:

```text
Current State
     │
     ▼
Recovery Attempt
     │
     X
   Failure
     │
     ▼
Rollback Journal
     │
     ▼
Previous State
```

Rollback restores the state that existed before the transaction.

---

## 11. Package Rejection

Tampered, malformed, incompatible, or otherwise invalid packages are rejected.

The system records:

```text
PACKAGE_REJECTED
```

A rejected package must not partially modify the world.

---

## 12. User Packages

OVERRIDE can distinguish packages created through the user-facing recovery workflow.

Such operations may generate:

```text
USER_PACKAGE
```

This allows recovery activity to be distinguished from ordinary system state changes.

---

## 13. Permissions

Recovery operations require root privileges.

Non-root attempts are rejected:

```text
error: root privileges required
```

Supported forms include:

```text
sudo get kernel
sudo load kernel
```

or, while persistent root mode is active:

```text
get kernel
load kernel
```

---

## 14. Idempotence

Repeated recovery of an already-correct system should not unnecessarily alter its logical state.

For example:

```text
sudo load kernel
sudo load kernel
```

should leave the system in the same valid kernel state.

This makes recovery predictable and easier to test.

---

## 15. Isolation

Recovery is scoped to the current world.

It must not silently modify:

* Other worlds
* Host filesystem state
* Unrelated persistent state

Recovery tests therefore use isolated application data locations.

---

## 16. Recovery Events

Important recovery transitions include:

```text
KERNEL_RESTORED
VFS_RESTORED
PACKAGE_REJECTED
USER_PACKAGE
FAULT_RESOLVED
```

These events provide an observable history of recovery operations.

---

## 17. Design Goals

The recovery subsystem prioritizes:

* **Safety** — validate before mutation.
* **Atomicity** — apply changes transactionally.
* **Rollback** — failed operations restore previous state.
* **Integrity** — detect modified packages.
* **Isolation** — affect only the intended world.
* **Idempotence** — repeated valid recovery remains predictable.
* **Preservation** — user data survives rootfs restoration.
