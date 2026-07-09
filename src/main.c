#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <linux/kvm.h>

#define GUEST_MEM_SIZE (1 << 20) // 1 MB placeholder for guest memory size

static const unsigned char guest_code[] = {
    0xba,
    0xf8,
    0x03, /* mov $0x3f8, %dx    -- dx = COM1 I/O port */
    0x00,
    0xd8, /* add %bl, %al       -- al += bl */
    0x04,
    '0',  /* add $'0' (0x30), %al  -- convert to ASCII digit */
    0xee, /* out %al, (%dx)     -- write digit to serial */
    0xb0,
    '\n', /* mov $'\n', %al */
    0xee, /* out %al, (%dx)     -- write newline */
    0xf4, /* hlt */
};

struct vm
{
    int fd;
    int sys_fd;
    void *mem;
    size_t mem_size;
};

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

struct vcpu
{
    int fd;
    struct kvm_run *kvm_run;
    int kvm_run_size;
};

void vcpu_setup_regs(struct vcpu *vcpu)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;

    // read current sregs from vCPU
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
    {
        perror("KVM_GET_SREGS");
        exit(1);
    }
    // set code segment to 0, base to 0, so code execution starts at physical address 0x0000
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    // apply sregs changes back to the vCPU
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
    {
        perror("KVM_SET_SREGS");
        exit(1);
    }

    memset(&regs, 0, sizeof(regs));
    regs.rip = 0x0000; // start execution at physical address 0x0000
    regs.rflags = 0x2; // set reserved bit 1 as required by x86 architecture
    regs.rax = 2;
    regs.rbx = 2;
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
    {
        perror("KVM_SET_REGS");
        exit(1);
    }
}

void vcpu_init(struct vm *vm, struct vcpu *vcpu)
{
    int vcpu_mmap_size;

    // create a vCPU instance
    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, 0);
    if (vcpu->fd < 0)
    {
        perror("KVM_CREATE_VCPU");
        exit(1);
    }

    // get size of kvm_run struct for this vCPU
    vcpu_mmap_size = ioctl(vm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu_mmap_size <= 0)
    {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        exit(1);
    }

    // mmap kvm_run struct for this vCPU
    vcpu->kvm_run = mmap(NULL, vcpu_mmap_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, vcpu->fd, 0);
    if (vcpu->kvm_run == MAP_FAILED)
    {
        perror("mmap kvm_run");
        exit(1);
    }
    vcpu->kvm_run_size = vcpu_mmap_size;
}

int vcpu_run(struct vcpu *vcpu)
{
    struct kvm_regs regs;

    for (;;)
    {
        // guest runs continuously until exits to the host, e.g., due to a halt instruction or an I/O operation
        if (ioctl(vcpu->fd, KVM_RUN, 0) < 0)
        {
            perror("KVM_RUN");
            exit(1);
        }

        // host inspect exit reason and handle accordingly, e.g., halt, I/O, or other exit reasons
        switch (vcpu->kvm_run->exit_reason)
        {
        case KVM_EXIT_IO:
            if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) < 0)
            {
                perror("KVM_GET_REGS");
                exit(1);
            }
            printf("KVM_EXIT_IO: port=0x%x al=0x%llx\n",
                   vcpu->kvm_run->io.port, regs.rax & 0xff);
            continue; // TODO: real port emulation in phase 4
        case KVM_EXIT_HLT:
            printf("KVM_EXIT_HLT\n");
            return 0;
        default:
            fprintf(stderr, "Unhandled exit reason: %d\n",
                    vcpu->kvm_run->exit_reason);
            exit(1);
        }
    }
}

void vm_cleanup(struct vm *vm)
{
    munmap(vm->mem, vm->mem_size);
    close(vm->fd);
    close(vm->sys_fd);
}

void vcpu_cleanup(struct vcpu *vcpu)
{
    munmap(vcpu->kvm_run, vcpu->kvm_run_size);
    close(vcpu->fd);
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

int main()
{
    struct vm vm;
    struct vcpu vcpu;

    vm_init(&vm, GUEST_MEM_SIZE);
    load_payload(&vm, guest_code, sizeof(guest_code));
    vcpu_init(&vm, &vcpu);
    vcpu_setup_regs(&vcpu);
    vcpu_run(&vcpu);

    vcpu_cleanup(&vcpu);
    vm_cleanup(&vm);
    return 0;
}