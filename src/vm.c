#define _POSIX_C_SOURCE 200809L
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <asm/bootparam.h>

// E820 memory region types (Linux boot protocol §4.3)
#define E820_TYPE_RAM 1
#define E820_TYPE_RESERVED 2

void vm_init(struct vm *vm, size_t mem_size)
{
    int api_version;
    struct kvm_userspace_memory_region memreg;

    // open /dev/kvm and check version
    vm->sys_fd = open("/dev/kvm", O_RDWR);
    if (vm->sys_fd < 0)
    {
        perror("open /dev/kvm");
        exit(1);
    }
    api_version = ioctl(vm->sys_fd, KVM_GET_API_VERSION, 0);
    if (api_version < 0)
    {
        perror("KVM_GET_API_VERSION");
        exit(1);
    }
    if (api_version != KVM_API_VERSION)
    {
        fprintf(stderr, "Got KVM api version %d, expected %d\n",
                api_version, KVM_API_VERSION);
        exit(1);
    }

    // create a VM instance
    vm->fd = ioctl(vm->sys_fd, KVM_CREATE_VM, 0);
    if (vm->fd < 0)
    {
        perror("KVM_CREATE_VM");
        exit(1);
    }

    // Reserve a 3-page TSS area just below the 4 GB boundary
    // as Intel VMX requires
    if (ioctl(vm->fd, KVM_SET_TSS_ADDR, 0xfffbd000u) < 0)
    {
        perror("KVM_SET_TSS_ADDR");
        exit(1);
    }

    if (ioctl(vm->fd, KVM_CREATE_IRQCHIP, 0) < 0)
    {
        perror("KVM_CREATE_IRQCHIP");
        exit(1);
    }
    struct kvm_pit_config pit = {.flags = 0};
    if (ioctl(vm->fd, KVM_CREATE_PIT2, &pit) < 0)
    {
        perror("KVM_CREATE_PIT2");
        exit(1);
    }

    // mmap the guest memory region
    vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vm->mem == MAP_FAILED)
    {
        perror("mmap mem");
        exit(1);
    }

    memreg.slot = 0;
    memreg.flags = 0;
    memreg.guest_phys_addr = 0;
    memreg.memory_size = mem_size;
    memreg.userspace_addr = (unsigned long)vm->mem;
    if (ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, &memreg) < 0)
    {
        perror("KVM_SET_USER_MEMORY_REGION");
        exit(1);
    }
    vm->mem_size = mem_size;
}

void vm_cleanup(struct vm *vm)
{
    munmap(vm->mem, vm->mem_size);
    close(vm->fd);
    close(vm->sys_fd);
}

void load_payload(struct vm *vm, const void *payload, size_t size)
{
    if (size > vm->mem_size)
    {
        fprintf(stderr, "payload (%zu bytes) larger than guest memory (%zu bytes)\n",
                size, vm->mem_size);
        exit(1);
    }

    memcpy(vm->mem, payload, size);

    if (memcmp(vm->mem, payload, size) != 0)
    {
        fprintf(stderr, "guest memory readback mismatch after copy\n");
        exit(1);
    }
}

void load_payload_from_file(struct vm *vm, const char *path)
{
    load_payload_from_file_at(vm, path, 0);
}

void load_payload_from_file_at(struct vm *vm, const char *path, size_t offset)
{
    // open file
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        perror(path);
        exit(1);
    }
    // move cursor to end to measure size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    // reset cursor back to start
    fseek(file, 0, SEEK_SET);

    if (size < 0 || (size_t)size > vm->mem_size)
    {
        fprintf(stderr, "file size (%ld bytes) larger than guest memory (%zu bytes)\n",
                size, vm->mem_size);
        fclose(file);
        exit(1);
    }

    // read file into memory
    size_t nread = fread((unsigned char *)vm->mem + offset, 1, (size_t)size, file);
    fclose(file);
    if (nread != (size_t)size)
    {
        fprintf(stderr, "failed to read entire file into guest memory\n");
        exit(1);
    }
}

static size_t align_up(size_t val, size_t align)
{
    return (val + align - 1) & ~(align - 1);
}

void load_kernel_bzimage(struct vm *vm, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        exit(1);
    }

    // setup_sects lives at byte 0x1F1 of the file.
    // If it is zero the spec says to treat it as 4.
    uint8_t setup_sects = 0;
    if (fseek(f, 0x1F1, SEEK_SET) != 0 ||
        fread(&setup_sects, 1, 1, f) != 1)
    {
        fprintf(stderr, "%s: failed to read setup_sects\n", path);
        fclose(f);
        exit(1);
    }
    if (setup_sects == 0)
        setup_sects = 4;

    // sanity check HdrS magic
    uint32_t magic = 0;
    if (fseek(f, 0x202, SEEK_SET) != 0 ||
        fread(&magic, sizeof(magic), 1, f) != 1 ||
        magic != 0x53726448U) // "HdrS" in little-endian
    {
        fprintf(stderr, "%s: missing HdrS magic, not a valid bzImage\n", path);
        fclose(f);
        exit(1);
    }

    size_t pm_offset = (size_t)(setup_sects + 1) * 512;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 0 || (size_t)fsize <= pm_offset)
    {
        fprintf(stderr, "%s: file too small to contain protected-mode kernel\n", path);
        fclose(f);
        exit(1);
    }
    size_t pm_size = (size_t)fsize - pm_offset;

    if (LINUX_KERNEL_ADDR + pm_size > vm->mem_size)
    {
        fprintf(stderr, "kernel protected-mode image (%zu bytes) overflows guest RAM\n",
                pm_size);
        fclose(f);
        exit(1);
    }

    fseek(f, (long)pm_offset, SEEK_SET);
    size_t n = fread((unsigned char *)vm->mem + LINUX_KERNEL_ADDR, 1, pm_size, f);
    fclose(f);
    if (n != pm_size)
    {
        fprintf(stderr, "%s: short read loading protected-mode kernel\n", path);
        exit(1);
    }
}

void load_initramfs(struct vm *vm, const char *path,
                    uint64_t *load_addr, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0)
    {
        fprintf(stderr, "%s: initramfs is empty or unreadable\n", path);
        fclose(f);
        exit(1);
    }

    // Place initramfs at the top of guest RAM, aligned to a 4 KB boundary.
    size_t file_size = (size_t)fsize;
    size_t aligned = align_up(file_size, 0x1000);
    if (aligned > vm->mem_size - LINUX_KERNEL_ADDR)
    {
        fprintf(stderr, "initramfs (%zu bytes) too large to fit in guest RAM\n",
                file_size);
        fclose(f);
        exit(1);
    }
    uint64_t addr = (uint64_t)(vm->mem_size - aligned);

    size_t n = fread((unsigned char *)vm->mem + addr, 1, file_size, f);
    fclose(f);
    if (n != file_size)
    {
        fprintf(stderr, "%s: short read loading initramfs\n", path);
        exit(1);
    }

    // write your guest physical address and byte count into *load_addr
    // and *size so the caller can record them in boot_params.
    *load_addr = addr;
    *size = (uint32_t)file_size;
}

// Construct the boot_params ("zero page") at guest physical 0x10000.
//
// The kernel reads this struct before doing anything else.  We:
//   1. Zero the entire region.
//   2. Copy the setup_header verbatim from the bzImage (it sits at the
//      same offset 0x1F1 in both the file and the struct).
//   3. Override the fields the boot protocol requires a bootloader to set.
//   4. Write the null-terminated command line at LINUX_CMDLINE_ADDR.
//   5. Populate a minimal E820 map that reflects the guest's actual RAM.
//
// The command line should include at least "console=ttyS0,115200" so the
// kernel's output is visible via the serial device on port 0x3F8 (COM1).
void setup_boot_params(struct vm *vm, const char *bzimage_path,
                       const char *cmdline,
                       uint64_t initramfs_addr, uint32_t initramfs_size)
{
    struct boot_params *bp =
        (struct boot_params *)((unsigned char *)vm->mem + LINUX_BOOT_PARAMS_ADDR);
    memset(bp, 0, sizeof(*bp));

    FILE *f = fopen(bzimage_path, "rb");
    if (!f)
    {
        perror(bzimage_path);
        exit(1);
    }
    // same offset 0x1F1 in bzImage and boot params struct place
    if (fseek(f, 0x1F1, SEEK_SET) != 0 ||
        fread(&bp->hdr, sizeof(bp->hdr), 1, f) != 1)
    {
        fprintf(stderr, "%s: failed to read setup_header into boot_params\n",
                bzimage_path);
        fclose(f);
        exit(1);
    }
    fclose(f);

    if (bp->hdr.header != 0x53726448U)
    {
        fprintf(stderr, "setup_boot_params: HdrS magic mismatch\n");
        exit(1);
    }
    if (bp->hdr.version < 0x0202)
    {
        fprintf(stderr, "setup_boot_params: boot protocol 0x%04x too old"
                        " (need >= 2.02 for cmd_line_ptr)\n",
                bp->hdr.version);
        exit(1);
    }

    bp->hdr.type_of_loader = 0xFF;
    bp->hdr.loadflags |= LOADED_HIGH;
    bp->hdr.loadflags |= CAN_USE_HEAP;
    bp->hdr.heap_end_ptr = 0x8FF0;
    bp->hdr.code32_start = (uint32_t)LINUX_KERNEL_ADDR;
    bp->hdr.ramdisk_image = (uint32_t)initramfs_addr;
    bp->hdr.ramdisk_size = initramfs_size;

    // Write the command line and record its address.
    size_t cmdlen = strnlen(cmdline, 4095);
    char *gcmdline = (char *)((unsigned char *)vm->mem + LINUX_CMDLINE_ADDR);
    memcpy(gcmdline, cmdline, cmdlen);
    gcmdline[cmdlen] = '\0';
    bp->hdr.cmd_line_ptr = (uint32_t)LINUX_CMDLINE_ADDR;
    bp->hdr.cmdline_size = (uint32_t)cmdlen;

    // E820 physical memory map
    bp->e820_table[0].addr = 0x00000000ULL;
    bp->e820_table[0].size = 0x0009F000ULL;
    bp->e820_table[0].type = E820_TYPE_RAM;

    bp->e820_table[1].addr = 0x000A0000ULL;
    bp->e820_table[1].size = 0x00060000ULL;
    bp->e820_table[1].type = E820_TYPE_RESERVED;

    bp->e820_table[2].addr = 0x00100000ULL;
    bp->e820_table[2].size = (uint64_t)vm->mem_size - 0x00100000ULL;
    bp->e820_table[2].type = E820_TYPE_RAM;

    bp->e820_entries = 3;
}
