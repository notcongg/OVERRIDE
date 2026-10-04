# OVERRIDE Testing

> Test architecture and validation strategy.

## Overview

OVERRIDE uses automated tests to verify behavior across its major subsystems.

Tests cover both normal operation and failure scenarios.

## Test Areas

The test suite covers:

* CLI commands
* Parser
* Dispatcher
* Authentication
* Root mode
* VFS
* Worlds
* Kernel
* Rootfs
* Recovery
* Persistence
* Events
* Isolation

## Recovery Tests

Recovery tests cover the complete workflow:

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
Commit / Rollback
```

Important cases include:

* Missing packages
* Invalid packages
* Tampered packages
* Kernel deletion
* Rootfs corruption
* Successful restoration
* Failed restoration
* Rollback
* Repeated recovery
* User-data preservation

## Privilege Tests

Non-root users must not be able to perform privileged recovery operations.

```text
get kernel
load kernel
```

must be rejected.

Privileged execution is tested through:

```text
sudo get kernel
sudo load kernel
```

Persistent root mode is also tested:

```text
sudo -a
get kernel
load kernel
sudo -e
```

## Isolation Tests

Tests verify that state does not leak between worlds.

```text
World A
   X
World B
```

They also verify that OVERRIDE operations do not modify unrelated host state.

## Persistence Tests

Persistence tests verify:

* State saving
* State reloading
* World reconstruction
* World isolation
* Configuration persistence
* Recovery package persistence
* Relaunch behavior

## Transaction Tests

Recovery transactions are tested for both successful and failed operations.

```text
Begin
 │
 ├── Apply
 │
 ├── Success ──► Commit
 │
 └── Failure ──► Rollback
```

A failed transaction must restore the previous logical state.

## Idempotence Tests

Repeated operations should produce stable results.

Examples:

```text
get kernel
get kernel
```

and:

```text
load kernel
load kernel
```

must not cause unexpected state changes.

## Current Validation

Current automated validation:

```text
mvp_test ALL_TESTS_OK checks=1611 failures=0
```

CTest:

```text
4/4 PASS
```

The complete Release configuration is also verified through CMake.

## Regression Testing

New features should include coverage for:

1. Normal behavior
2. Invalid input
3. Permission boundaries
4. Persistence
5. Isolation
6. Failure handling
7. Interaction with existing commands

Existing tests must remain passing after changes.

## Philosophy

Tests should verify observable behavior rather than implementation details.

The goal is simple:

> **If the system changes, previously verified behavior should not silently break.**
