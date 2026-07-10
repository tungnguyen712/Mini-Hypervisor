#include "vcpu.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

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
    regs.rax = 3;
    regs.rbx = 4;
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

    // mmap kvm_run (shared memory region between the kernel and the user-space vCPU)
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
            handle_io(vcpu);
            continue;
        case KVM_EXIT_MMIO:
            handle_mmio(vcpu);
            continue;
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

void vcpu_cleanup(struct vcpu *vcpu)
{
    munmap(vcpu->kvm_run, vcpu->kvm_run_size);
    close(vcpu->fd);
}