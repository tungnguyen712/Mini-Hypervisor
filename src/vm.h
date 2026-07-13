#ifndef VM_H
#define VM_H

#include <stddef.h>
#include <stdint.h>

struct vm
{
    int fd;
    int sys_fd;
    void *mem;
    size_t mem_size;
};

// Guest physical addresses used by the Linux boot protocol.
#define LINUX_BOOT_PARAMS_ADDR 0x10000UL
#define LINUX_CMDLINE_ADDR 0x20000UL
#define LINUX_KERNEL_ADDR 0x100000UL

void vm_init(struct vm *vm, size_t mem_size);
void vm_cleanup(struct vm *vm);
void load_payload(struct vm *vm, const void *payload, size_t size);
void load_payload_from_file(struct vm *vm, const char *path);
void load_payload_from_file_at(struct vm *vm, const char *path, size_t offset);

// compiled Linux kernel as a single file
// replace previous hand-written guest/payloads/vcpu_long_mode.asm
void load_kernel_bzimage(struct vm *vm, const char *path);

// initframs: initial RAM filesystem; kernel use to find its first programs (init, PID, etc.)
void load_initramfs(struct vm *vm, const char *path,
                    uint64_t *load_addr, uint32_t *size);

// contains the boot parameters required by the Linux kernel at boot time,
void setup_boot_params(struct vm *vm, const char *bzimage_path,
                       const char *cmdline,
                       uint64_t initramfs_addr, uint32_t initramfs_size);

#endif // VM_H