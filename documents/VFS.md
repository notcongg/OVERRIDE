# OVERRIDE VFS

> Virtual filesystem architecture and behavior.

## Overview

The OVERRIDE VFS provides filesystem semantics inside the simulated environment.

It is completely separate from the host filesystem.

```text
VFS
├── Paths
├── Files
├── Directories
├── Metadata
├── Permissions
└── Integrity
```

## Paths

Paths are resolved inside the active simulated world.

Examples:

```text
/
 /boot
 /etc
 /home
 /tmp
```

The shell maintains the current virtual working directory.

## Files

The VFS supports basic file operations:

* Create
* Read
* Write
* Delete

Example commands:

```text
touch /home/test.txt
cat /home/test.txt
rm /home/test.txt
```

## Directories

Directories provide the filesystem hierarchy.

```text
mkdir /home/test
cd /home/test
ls
```

## Permissions

Filesystem operations may be restricted by the simulated user's privileges.

System paths may require root privileges.

These permissions belong to OVERRIDE's simulated environment and do not modify Windows permissions.

## Integrity

The VFS can produce a logical digest representing filesystem state.

Recovery packages use this information to verify that expected filesystem data has not been modified.

```text
VFS State
    │
    ▼
Digest
    │
    ▼
Recovery Metadata
```

## Rootfs Recovery

Rootfs recovery can restore the known-good filesystem baseline.

The operation may:

* Restore system files
* Recreate required directories
* Repair corrupted configuration
* Preserve user-created data outside the baseline

See [`RECOVERY.md`](RECOVERY.md).

## World Isolation

Every world owns its own VFS state.

```text
World A ──► VFS A

World B ──► VFS B
```

Operations in one world must not modify another world.

The VFS must also remain isolated from the host filesystem.

## Design Rules

* Never treat host paths as VFS paths.
* Keep path resolution deterministic.
* Validate operations before mutation.
* Preserve world isolation.
* Use transactional behavior during recovery.
