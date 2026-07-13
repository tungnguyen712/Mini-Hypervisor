# Mini-Hypervisor

A minimal type-2 hypervisor for x86-64 Linux, built directly on the raw
`/dev/kvm` ioctl API (no libvirt, no QEMU, no device-model framework). It
boots a real Linux kernel + initramfs to an interactive shell over an
emulated 16550 UART.

## What it does

- Drives KVM directly: `KVM_CREATE_VM`, `KVM_CREATE_VCPU`,
  `KVM_SET_USER_MEMORY_REGION`, `KVM_CREATE_IRQCHIP` + `KVM_CREATE_PIT2`
  (in-kernel PIC/PIT), CPUID passthrough, and the Linux 32-bit boot
  protocol entry point.
- Handles `KVM_EXIT_IO`, `KVM_EXIT_MMIO`, `KVM_EXIT_HLT`, and
  `KVM_EXIT_SHUTDOWN` by hand.
- Emulates a 16550 UART (COM1) with real register state (IER/LCR/MCR/SCR,
  not hardcoded stubs) and interrupt-driven RX and TX — including firing
  `KVM_IRQ_LINE` asynchronously from a stdin-reader thread, since a halted
  vCPU under a fully in-kernel irqchip blocks inside the kernel and never
  returns `KVM_EXIT_HLT` to userspace.
- Boots an actual Linux `bzImage` + a small busybox initramfs to a live,
  interactive `ash` shell over the serial console.

## Requirements

- A Linux host with `/dev/kvm` accessible 
- `gcc`, `nasm`, `make`.
- `cpio` and `gzip` (only needed for the `make initramfs` step).
- A static `busybox` binary on `PATH` — on Debian/Ubuntu:
  `sudo apt install busybox-static`.

## Setup

Two large binaries are required to run but are **not** tracked in this
repo (see `.gitignore`): a Linux kernel image (`bzImage`) and a packaged
initramfs (`initramfs.cpio.gz`).

1. **Kernel image.** Copy any bzImage-format Linux kernel to the repo root
   as `bzImage`.

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

`mini_hv` boots the kernel with `console=ttyS0,115200 rdinit=/init`, so the
entire boot log and an interactive shell prompt appear directly in your
terminal. Type normally once you see the `~ #` prompt; input is delivered
to the guest via IRQ-driven UART emulation, so it responds in
real time.

Ctrl+C, job control, and window-size negotiation are not wired up.

## Tests

```sh
make test  # basic /dev/kvm capability check (API version, extensions)
```