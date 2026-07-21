# Mini-Hypervisor

A minimal type-2 hypervisor for x86-64 Linux, built directly on the raw
`/dev/kvm` ioctl API (no libvirt, no QEMU). One daemon (`mini_hv`) runs on
the **server** (the host machine); **clients** (you, or tenants you give a
token to) connect over a UNIX socket to create/list/destroy VMs and reach
them over the network.

## What it does

- Boots real Linux `bzImage` + busybox-initramfs guests directly on KVM,
  each in its own vCPU thread(s).
- Per-VM serial UART (`vm-logs/vm-<id>.log`), virtio-net NIC, and a
  control-plane API over a UNIX socket (`src/server.c`).
- Token auth + per-tenant ownership on every VM.

## Requirements

- Linux host with `/dev/kvm` accessible, `gcc`, `nasm`, `make`, `cpio`,
  `gzip`, `iptables`.
- Static `busybox` on `PATH`: `sudo apt install busybox-static`.

---

## Server setup (do this once, on the host)

**1. Get a kernel.** Needs `CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y` for
networking to work. Check:

```sh
zcat /proc/config.gz 2>/dev/null | grep VIRTIO_MMIO_CMDLINE_DEVICES
```

If missing, build one:

```sh
curl -O https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-<version>.tar.xz
tar xf linux-<version>.tar.xz && cd linux-<version>
make defconfig && make kvm_guest.config
./scripts/config --enable CONFIG_VIRTIO_MMIO --enable CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES --enable CONFIG_VIRTIO_NET
make olddefconfig && make -j"$(nproc)" bzImage
cp arch/x86/boot/bzImage /path/to/Mini-Hypervisor/bzImage
```

No networking needed? Any kernel works: `cp /boot/vmlinuz-$(uname -r) bzImage`.

**2. Build the initramfs**:

```sh
make initramfs
```

**3. Build and run the daemon.** `make run` also mints a local dev token on
first run:

```sh
make
make run   # -> ./mini_hv --tokens mini_hv.tokens
```

**4. Provision a tenant** (mint them their own token):

```sh
./mhv-token tokens.conf alice   # appends "<token> alice"
```

**5. Let remote clients in.** Give a tenant SSH
access to the host; they tunnel the UNIX socket directly:

```sh
ssh -L /local/mini_hv.sock:/remote/mini_hv.sock user@host
```

---

## Client usage (a tenant with a token)

Talk to the socket with `socat` (or any UNIX-socket client). Every
connection must `AUTH` first:

```sh
socat - UNIX-CONNECT:mini_hv.sock
AUTH <your-token> # OK authenticated as alice
CREATE kernel=bzImage initramfs=initramfs.cpio.gz # OK id=1
LIST # OK count=1 / id=1 state=running
STATUS 1 # OK id=1 state=running ... net=tap1
DESTROY 1 # OK id=1 destroyed
```

**Reach a VM from outside** by forwarding a port:

```sh
FORWARD 1 8000 80 # OK forwarded host_port=8000 -> vm=1 guest_port=80
UNFORWARD 1 8000 # OK unforwarded host_port=8000
```

---

## Networking

Each VM: virtio-net NIC on `172.30.<id>.0/24` (host `.1`, guest `.2`),
NAT'd to the internet, isolated from every other VM's subnet
(`tap_isolate_vm` in `src/tap.c`)

## Tests

```sh
make test      # /dev/kvm capability check
make test-api  # throwaway daemon + tokens, exercises the full API + ownership checks
```