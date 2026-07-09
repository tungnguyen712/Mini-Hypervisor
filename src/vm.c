#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

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