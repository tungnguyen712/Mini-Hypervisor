# Mini-Hypervisor

A minimal type-2 hypervisor for x86-64 Linux, built directly on the raw
`/dev/kvm` ioctl API (no libvirt, no QEMU, no device-model framework). It
runs as a long-lived control-plane daemon: create, list, inspect, and
destroy Linux kernel + initramfs guests over a UNIX socket, with each guest
boots and runs concurrently under its own vCPU thread(s).

## What it does

- Drives KVM directly: `KVM_CREATE_VM`, `KVM_CREATE_VCPU`,
  `KVM_SET_USER_MEMORY_REGION`, `KVM_CREATE_IRQCHIP` + `KVM_CREATE_PIT2`
  (in-kernel PIC/PIT), CPUID passthrough, and the Linux 32-bit boot
  protocol entry point.
- Handles `KVM_EXIT_IO`, `KVM_EXIT_MMIO`, `KVM_EXIT_HLT`, and
  `KVM_EXIT_SHUTDOWN` by hand.
- Emulates a 16550 UART (COM1) per VM, with real register state
  (IER/LCR/MCR/SCR, not hardcoded stubs) and interrupt-driven TX. Guest
  serial *output* is captured to a per-VM log file under `vm-logs/`; there
  is no interactive input/console-attach in this phase (the daemon has no
  controlling terminal to attach one to).
- Boots an actual Linux `bzImage` + a small busybox initramfs per VM.
- A control-plane API server (`src/server.c`) listens on a UNIX domain
  socket and speaks a small line-based text protocol to create, list,
  inspect, and destroy VMs. A fixed-size registry (`src/registry.c`) tracks
  each VM's state, and a per-VM supervisor thread joins that VM's vCPU
  thread(s) in the background so the API never blocks on any single VM.

## Requirements

- A Linux host with `/dev/kvm` accessible 
- `gcc`, `nasm`, `make`.
- `cpio` and `gzip` (only needed for the `make initramfs` step).
- A static `busybox` binary on `PATH` — on Debian/Ubuntu:
  `sudo apt install busybox-static`.

## Setup

At least one kernel image + initramfs pair is needed to boot a guest, but
neither is tracked in this repo (see `.gitignore`) — a VM's kernel/initramfs
paths are supplied per-request when you `CREATE` it, not hardcoded.

1. **Kernel image.** Any bzImage-format Linux kernel. A copy in the repo
   root as `bzImage` matches the examples below:

   ```sh
   cp /boot/vmlinuz-$(uname -r) bzImage
   ```

2. **Initramfs.** The actual source is tracked at `guest/initramfs/init`
   (a busybox `cttyhack sh` init script). Build the packaged image with:

   ```sh
   make initramfs
   ```

## Build & run

```sh
make        # builds ./mini_hv (and the guest/payloads/*.bin test payloads)
make run    # equivalent to: ./mini_hv
```

`mini_hv` is a daemon: it binds a UNIX socket and blocks forever, waiting for control-
plane requests. It prints nothing to the terminal per guest, boot logs and
console output for each VM are captured to `vm-logs/vm-<id>.log`.

Talk to it with any UNIX-socket client, e.g. `socat`:

```sh
socat - UNIX-CONNECT:mini_hv.sock
CREATE kernel=bzImage initramfs=initramfs.cpio.gz
# OK id=1
LIST
# OK count=1
# id=1 state=running
STATUS 1
# OK id=1 state=running kernel=bzImage initramfs=initramfs.cpio.gz disk=-
DESTROY 1
# OK id=1 destroyed
```

## Tests

```sh
make test      # basic /dev/kvm capability check (API version, extensions)
make test-api  # starts a throwaway daemon and exercises CREATE/LIST/STATUS/
               # DESTROY over its socket (requires bzImage/initramfs.cpio.gz)
```