# Contributing to OVERRIDE

Thanks for your interest in contributing to OVERRIDE.

OVERRIDE is an experimental simulated operating environment, so contributions should preserve its isolation, determinism, and system-oriented design.

## Before You Start

Before making changes:

1. Read [`README.md`](README.md).
2. Check the relevant documentation in [`documents/`](documents/).
3. Search existing issues or code before introducing a new implementation.
4. Keep changes focused.

## Development

OVERRIDE is built with:

* C++17
* CMake
* Standard library

Build the project with:

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Testing

Run the test suite before submitting changes:

```bash
ctest --test-dir build --output-on-failure
```

For changes affecting core behavior, also run the project's full test executable when applicable.

All existing tests should remain passing.

## Code Guidelines

Prefer:

* Clear and explicit code
* Small, focused changes
* Existing abstractions over duplicated implementations
* Deterministic behavior
* Proper error handling
* Tests for new behavior

Avoid:

* Unnecessary dependencies
* Duplicating existing command logic
* Host-system side effects
* Unrelated refactors
* Silent behavior changes

## System Safety

OVERRIDE simulates an operating environment.

Changes must preserve the boundary between:

```text
Host OS
   X
OVERRIDE
```

Code should not unexpectedly modify host files, permissions, services, or other host resources.

## Commands and Features

When adding a command:

1. Implement it through the existing command architecture.
2. Add help/documentation where appropriate.
3. Enforce privilege requirements.
4. Test valid and invalid input.
5. Test interactions with relevant subsystems.

Aliases should reuse existing implementations instead of creating duplicate command paths.

## Recovery Changes

Recovery-related changes require extra care.

They should preserve:

* Package integrity validation
* Transactional behavior
* Rollback
* World isolation
* User-data preservation
* Idempotence

A failed recovery operation must not leave partial state.

See [`documents/RECOVERY.md`](documents/RECOVERY.md).

## Documentation

Update documentation when behavior changes.

Relevant documentation is located in:

```text
documents/
├── ARCHITECTURE.md
├── SYSTEMS.md
├── COMMANDS.md
├── RECOVERY.md
├── VFS.md
├── WORLDS.md
└── TESTING.md
```

## Pull Requests

A good pull request should include:

* A clear description of the change
* Why the change is needed
* Relevant tests
* Documentation updates when necessary

Keep pull requests focused on one feature or fix whenever possible.

## Commit Messages

Use concise commit messages that describe the change.

Examples:

```text
(feat) add rootfs recovery
(fix) prevent invalid package loading
(test) add recovery rollback coverage
(docs) update command reference
```

## Final Checklist

Before submitting:

```text
[ ] Code builds successfully
[ ] Tests pass
[ ] No host-system side effects
[ ] Privilege boundaries are preserved
[ ] World isolation is preserved
[ ] Documentation is updated
[ ] Changes are focused
```

---

By contributing to OVERRIDE, you agree that your contributions are provided under the project's license.
