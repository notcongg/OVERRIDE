# OVERRIDE VFS

> Virtual filesystem architecture and behavior.

## 1. Overview

The OVERRIDE VFS provides filesystem semantics inside the simulated environment.

It is independent from the host filesystem.

```text
VFS
├── Paths
├── Files
├── Directories
├── Metadata
├── Permissions
└── Integrity
```

---

## 2. Paths

Paths are resolved inside the current simulated world.

Examples:

```text
/
 /boot
 /etc
 /home
 /tmp
```

The current working directory is maintained by the shell session.

---

## 3. Files

The VFS supports basic file operations:

```text
create
read
write
delete
```

Typical commands include:

```text
touch <path>
cat <path>
rm <path>
```

---

## 4. Directories

Directories provide the hierarchy used by the simulated filesystem.

Common operations include:

```text
mkdir <path>
cd <path>
ls <path>
```

Parent directories must exist unless the operation explicitly creates them.

---

## 5. Permissions

Filesystem operations may be restricted according to the simulated user's privileges.

System-level paths may require root privileges.

The VFS permission model is part of OVERRIDE's simulation and does not grant or remove Windows permissions.

---

## 6. Integrity

The VFS can produce a logical digest representing filesystem state.

This digest is used by recovery packages to verify that expected filesystem data has not been altered.

```text
VFS State
   │
   ▼
Digest
   │
   ▼
Recovery Metadata
```

---

## 7. Recovery

Rootfs recovery restores the known-good baseline.

The recovery system may:

* Restore system files
* Recreate required directories
* Repair corrupted configuration
* Preserve user-created data outside the baseline

See [`RECOVERY.md`](RECOVERY.md).

---

## 8. Isolation

Each world owns its own VFS state.

```text
World A ──► VFS A

World B ──► VFS B
```

Operations in one world must not modify another world's filesystem.

The VFS must also remain isolated from the host filesystem.

---

## 9. Design Rules

* Never treat host paths as VFS paths.
* Keep path resolution deterministic.
* Validate filesystem operations before mutation.
* Preserve world isolation.
* Use transactional behavior for recovery operations.
