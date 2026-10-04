# Changelog

All notable changes to OVERRIDE are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

Changes for the next release will be documented here.

## [0.1.0]

### Added

* Simulated operating environment
* Virtual filesystem (VFS)
* Isolated worlds
* Kernel and rootfs simulation
* Process system
* Service system
* Authentication and privilege management
* Temporary `sudo <command>` execution
* Persistent root mode with `sudo -a`
* Root mode exit with `sudo -e`
* Sudo status with `sudo -st`
* Command help system
* Command aliases
* Simulated system clock
* Event system and ledger
* Persistent world state
* ORDC recovery packages
* Kernel recovery
* Rootfs recovery
* `sudo get kernel`
* `sudo get rootfs`
* `sudo get kernel,rootfs`
* `sudo load kernel`
* `sudo load rootfs`
* `sudo load kernel,rootfs`
* Recovery package integrity validation
* SHA-256 and VFS digest verification
* Atomic recovery package writes
* Transactional recovery
* Rollback support
* User-data preservation during rootfs recovery
* Recovery-related events
* World isolation

### Testing

* 1611 automated checks
* `mvp_test`: `ALL_TESTS_OK`
* Failures: `0`
* CTest: `4/4 PASS`
* Release build verified successfully

[Unreleased]: https://github.com/notcongg/OVERRIDE/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/notcongg/OVERRIDE/releases/tag/v0.1.0
