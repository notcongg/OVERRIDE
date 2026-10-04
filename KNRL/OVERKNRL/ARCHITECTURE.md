# OVERKNRL -- simulated machine layer

OVERKNRL is the virtual operating-system / machine layer of the OVERRIDE
simulation. It is **simulation state and project data only**. Nothing here
touches the real host: no host processes, files, kernel state, hardware,
network interfaces, permissions, or destructive host commands.

## Physical vs virtual -- do not confuse them

Physical repository (implementation / project data):

```text
E:\Code\OVERRIDE\KNRL\OVERKNRL\
```

Simulated filesystem (virtual state, in-memory, snapshotted):

```text
/
+-- bin        (init sh echo sleep netd logger app; simulated executables)
+-- sbin       (mount umount reboot shutdown modprobe lsmod sysctl init)
+-- boot       (kernel image, initramfs, boot.conf)
+-- kernel     (kernel.conf, symbols, modules.list)
+-- lib        (modules/*.ko, firmware)
+-- dev        (null zero console tty0 random net0 disk0 cpu0 metadata)
+-- proc       (cpuinfo meminfo uptime processes interrupts modules mounts)
+-- sys        (kernel devices thermal memory network)
+-- run        (lock state services/ pid/)
+-- etc        (node confs + hostname hosts resolv.conf fstab services.conf
+--             kernel.conf modules.conf override.conf)
+-- home
+-- root
+-- tmp
+-- usr        (bin/ sbin/ lib/ share/)
+-- var        (log/ cache/ lib/ run/)
```

Dependency map (enforced by the engine, visible via why/trace/events):

```text
/boot/kernel      -> bootability (KERNEL_LOAD; running kernels continue)
kernel.conf       -> boot (BOOTLOADER) + kernel-config fault when corrupted
/sbin/init|/bin   -> INIT phase (either suffices; losing both stops boot)
/lib/modules/*.ko -> module availability (DRIVER_INIT + module-load)
/dev/net0         -> iface/packet/ping   /dev/disk0 -> stored writes
/dev/console      -> boot console        other /dev/* -> inspectable metadata
/etc/<node>.conf  -> node configuration  /etc/* -> protected config area
/bin/* /sbin/*    -> exec + service-start binary resolution (exec bit)
/run/*            -> volatile runtime    /var/log/* -> diagnostics
```

Only meaningful dependencies propagate: deleting an arbitrary file never
destroys the OS; the paths above gate exactly their documented dependents.

Path mapping (documentation of intent, enforced by tests):

```text
/etc  -> E:\Code\OVERRIDE\KNRL\OVERKNRL\rootfs\etc\
/bin  -> E:\Code\OVERRIDE\KNRL\OVERKNRL\rootfs\bin\
/var  -> E:\Code\OVERRIDE\KNRL\OVERKNRL\rootfs\var\
...and so on for sbin, tmp, home, root, usr.
```

Rules:

* NEVER create or use `E:\etc`, `E:\bin`, `E:\var` or any other host-rooted
  path for OVERKNRL data.
* OVERKNRL must never write outside `E:\Code\OVERRIDE\KNRL\OVERKNRL\`
  for anything related to its simulated filesystem.
* In practice the engine goes further: the live virtual tree is **purely
  in-memory** (`Vfs`), so simulated `ls/cd/cat/mkdir/rm/touch/cp/mv/edit`
  cannot reach the host disk at all. The physical `rootfs/` tree below
  exists as auditable **seed data + documentation**, and a test asserts
  the in-memory defaults match it file-for-file.
* The `.ovr` scenario runner only *reads* script files (like `run`).

## Concept -> code map

```text
OVERKNRL concept        implemented in
--------------------    ----------------------------------------
kernel lifecycle        KNRL/src/system_faults.cpp (12-phase rebootNode/
                        startNode/haltNode with prerequisite gates)
kernel modules          Node::modules + module faults + /lib/modules images
boot pipeline           BOOT_PHASE events (PREPARE..RUNNING), BOOT_FAILED +
                        boot-failure fault; `kernel`/`dmesg`/`lsmod` views
rootfs / mounts         Node::filesystems + KNRL/src/system_vfs.cpp gating
virtual files           Vfs (KNRL/src/vfs.cpp, exec bit), state in System,
                        snapshotted; seed mirrored under rootfs/ (tested)
processes/threads       ProcessManager (process.cpp); threads + mailboxes
                        live inside SimProcess (same registry, snapshotted)
services                ServiceManager (service.cpp); binaries resolve via
                        node set or /bin|/sbin images
resources               Node fields (cpu/load, memory, storage, io, links)
network                 Network (network.cpp) + system_net.cpp; /dev/net0 gate
faults                  fault catalog + raise/resolve (system_faults.cpp)
events/causality        EventLog (event.cpp), single ledger; why = effective
                        root (never a trigger), trace = current incarnation
time/rng                Clock + seeded streams, snapshots carry rng state
checkpoints             History snapshots (nodes, links, procs, services, hw,
                        vfs, cwd, clock, seed, rng, entropy settings)
                        (runtime-only; .ord files carry world state + ledger)
persistence             ORDC/ (.ord docs) + KNRL worlds (slots, atomic save,
                        sessions/uptime) + achievements (global meta)
auth/permissions        KNRL account (PBKDF2 oup.ord, operator identity) +
                        VFS ACLs (ur/uw/or/ow/rtr/rtw) enforced by vfs ops
recovery packages       KNRL packages (engine-canonical kernel.ord/rootfs.ord
                        in <appdir>/recovery/) + System transactional loads
                        (rollback journal; overlay, never reset)
```

## Layering

```text
OVERSHELL  (input, parsing, dispatch, formatting, session state)
    |
    v
KNRL engine (world state + orchestration: System::tick/onTick/propagate)
    |
    v
OVERKNRL concepts (kernel, rootfs, modules, processes, services, resources)
    |
    v
SIMULATED MACHINE (per-node state; never the host)
```

Dependency direction is strictly downward: shell -> engine -> domain types.
The engine never includes shell/parser/scenario headers.
