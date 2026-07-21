#ifndef VM_H
#define VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

struct com1_device;
struct virtio_net_device;

struct vm
{
    int fd;
    int sys_fd;
    void *mem;
    size_t mem_size;

    int id;
    struct com1_device *com1;      // per-VM UART device
    struct virtio_net_device *net; // per-VM virtio-net device, NULL if net setup failed
    atomic_int stop_requested;
};

// Per-VM configuration supplied by the control-plane API (replaces the
// hardcoded flat-binary path from earlier phases).
struct vm_config
{
    char kernel_path[256];
    char initramfs_path[256];
    char disk_path[256]; // "" = unset. Phase 12 stub: plumbed through, unused.
};

// Guest physical addresses used by the Linux boot protocol.
#define LINUX_BOOT_PARAMS_ADDR 0x10000UL
#define LINUX_CMDLINE_ADDR 0x20000UL
#define LINUX_KERNEL_ADDR 0x100000UL

// All functions below return 0 on success, -1 on failure
int vm_init(struct vm *vm, size_t mem_size);
void vm_cleanup(struct vm *vm);
int load_payload(struct vm *vm, const void *payload, size_t size);
int load_payload_from_file(struct vm *vm, const char *path);
int load_payload_from_file_at(struct vm *vm, const char *path, size_t offset);

// compiled Linux kernel as a single file
// replace previous hand-written guest/payloads/vcpu_long_mode.asm
int load_kernel_bzimage(struct vm *vm, const char *path);

// initframs: initial RAM filesystem; kernel use this to find its first programs (init, PID, etc.)
int load_initramfs(struct vm *vm, const char *path,
                   uint64_t *load_addr, uint32_t *size);

// contains the boot parameters required by the Linux kernel at boot time
int setup_boot_params(struct vm *vm, const char *bzimage_path,
                      const char *cmdline,
                      uint64_t initramfs_addr, uint32_t initramfs_size);

// Orchestrates vm_init + load_kernel_bzimage + load_initramfs +
// setup_boot_params from a vm_config
int vm_setup(struct vm *vm, int id, const struct vm_config *cfg);

#endif // VM_H