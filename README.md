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
- Gives each VM a virtio-net NIC (`src/virtio_net.c`), backed by a host TAP
  device (`src/tap.c`) and NAT'd through the host's own network connection —
  see [Networking](#networking) below.

## Requirements

- A Linux host with `/dev/kvm` accessible 
- `gcc`, `nasm`, `make`.
- `cpio` and `gzip` (only needed for the `make initramfs` step).
- A static `busybox` binary on `PATH` — on Debian/Ubuntu:
  `sudo apt install busybox-static`.
- For guest networking: `iptables`, and the daemon needs to run with
  `CAP_NET_ADMIN` (in practice, `sudo ./mini_hv ...`) to create TAP devices
  and install NAT/port-forward rules.

## Setup

At least one kernel image + initramfs pair is needed to boot a guest, but
neither is tracked in this repo (see `.gitignore`) — a VM's kernel/initramfs
paths are supplied per-request when you `CREATE` it, not hardcoded.

1. **Kernel image.** A bzImage-format Linux kernel **with virtio-net
   support and, specifically, `CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y`** — this
   is the option that makes the kernel parse the `virtio_mmio.device=`
   command-line argument the daemon passes each guest to tell it where the
   NIC lives; most distro kernels have `CONFIG_VIRTIO_MMIO`/`_NET` but leave
   this specific sub-option *off*, in which case the guest boots fine but
   never sees a NIC. Check with:

   ```sh
   zcat /proc/config.gz 2>/dev/null | grep VIRTIO_MMIO_CMDLINE_DEVICES  # if running kernel == target kernel
   ```

   If it's missing, build a minimal kernel with it enabled (a few minutes
   with a reasonable `-j`):

   ```sh
   curl -O https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-<version>.tar.xz
   tar xf linux-<version>.tar.xz && cd linux-<version>
   make defconfig && make kvm_guest.config   # minimal-VM starting point
   ./scripts/config --enable CONFIG_VIRTIO_MMIO
   ./scripts/config --enable CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES
   ./scripts/config --enable CONFIG_VIRTIO_NET
   make olddefconfig
   make -j"$(nproc)" bzImage
   cp arch/x86/boot/bzImage /path/to/Mini-Hypervisor/bzImage
   ```

   If you don't need networking for a given test, any bzImage works (the
   NIC just won't attach) — see `cp /boot/vmlinuz-$(uname -r) bzImage` for
   the quickest option.

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
# OK id=1 state=running kernel=bzImage initramfs=initramfs.cpio.gz disk=- net=tap1
DESTROY 1
# OK id=1 destroyed
```

## Networking

Each VM gets a virtio-net NIC on `172.30.<octet>.0/24` (host `.1`, guest
`.2`, `<octet>` = VM id), assigned via kernel cmdline (no DHCP), NAT'd
through the host's own connection. Check `STATUS <id>`'s `net=tap<N>` field
to confirm it came up.

To make a VM reachable from outside (a friend, a remote client), forward a
port:

| Request | Success |
|---|---|
| `FORWARD <id> <host_port> <guest_port>` | `OK forwarded host_port=<n> -> vm=<id> guest_port=<n>` |
| `UNFORWARD <id> <host_port>` | `OK unforwarded host_port=<n>` |

If the host itself is behind NAT, like WSL2, reaching it externally needs one more hop outside this
daemon, e.g. a Windows `netsh interface portproxy` rule into WSL2's own IP,
which then hits the `FORWARD` rule above.

**VMs are isolated from each other.** Each is NAT'd to the internet and
reachable via its own `FORWARD` rules, but can't route directly to another
VM's subnet, enforced per-tap in the host's `FORWARD` chain (`tap.c`), not
by the guest OS.

## Tests

```sh
make test      # basic /dev/kvm capability check (API version, extensions)
make test-api  # starts a throwaway daemon and exercises CREATE/LIST/STATUS/
               # DESTROY over its socket (requires bzImage/initramfs.cpio.gz)
```