#define _DEFAULT_SOURCE
#include "vcpu.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/kvm.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

void vcpu_setup_regs(struct vcpu *vcpu, uint64_t rip)
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
    // set EFER.LME=1 so the guest can activate long mode via CR0.PG
    sregs.efer |= 0x100;
    // apply sregs changes back to the vCPU
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
    {
        perror("KVM_SET_SREGS");
        exit(1);
    }

    memset(&regs, 0, sizeof(regs));
    regs.rip = rip;         // start execution at the specified physical address
    regs.rbx = rip;         // pass load base to guest: BX = physical address where payload was loaded
    regs.rsp = rip + 0xFF0; // per-VCPU real-mode stack top, well within the VCPU's own 4KB page
    regs.rflags = 0x2;      // set reserved bit 1 as required by x86 architecture
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
    {
        perror("KVM_SET_REGS");
        exit(1);
    }
}

void vcpu_get_sregs(struct vcpu *vcpu, struct kvm_sregs *sregs)
{
    if (ioctl(vcpu->fd, KVM_GET_SREGS, sregs) < 0)
    {
        perror("KVM_GET_SREGS");
        exit(1);
    }
}

void vcpu_init(struct vm *vm, struct vcpu *vcpu, unsigned long vcpu_id)
{
    int vcpu_mmap_size;

    // create a vCPU instance
    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, vcpu_id);
    if (vcpu->fd < 0)
    {
        perror("KVM_CREATE_VCPU");
        exit(1);
    }
    vcpu->vm_fd = vm->fd;

    // Expose host-supported CPUID leaves to the guest
    {
        const int nent = 100;
        struct kvm_cpuid2 *cpuid = malloc(
            sizeof(*cpuid) + (size_t)nent * sizeof(cpuid->entries[0]));
        if (!cpuid)
        {
            perror("malloc cpuid");
            exit(1);
        }
        cpuid->nent = nent;
        if (ioctl(vm->sys_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
        {
            perror("KVM_GET_SUPPORTED_CPUID");
            free(cpuid);
            exit(1);
        }
        if (ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid) < 0)
        {
            perror("KVM_SET_CPUID2");
            free(cpuid);
            exit(1);
        }
        free(cpuid);
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
            // Guest CPU is idle. If COM1 has pending data and the guest has
            // receive interrupts enabled (IER.RDI), wake up the serial driver;
            // otherwise sleep and re-run.
            if (com1_rx_avail() && (com1_ier() & 0x01))
            {
                struct kvm_irq_level irq = {.irq = 4, .level = 1};
                ioctl(vcpu->vm_fd, KVM_IRQ_LINE, &irq);
                irq.level = 0;
                ioctl(vcpu->vm_fd, KVM_IRQ_LINE, &irq);
            }
            else
            {
                usleep(1000);
            }
            continue;
        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr, "KVM_EXIT_SHUTDOWN (triple fault)\n");
            return -1;
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

// Configure a VCPU for the 32-bit Linux boot protocol entry point.
void vcpu_setup_linux32_entry(struct vcpu *vcpu)
{
    struct kvm_sregs sregs;
    struct kvm_regs regs;

    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
    {
        perror("KVM_GET_SREGS");
        exit(1);
    }

    sregs.cs.base = 0;
    sregs.cs.limit = 0xFFFFFFFF;
    sregs.cs.selector = 0x10;
    sregs.cs.type = 0xA;
    sregs.cs.present = 1;
    sregs.cs.dpl = 0;
    sregs.cs.db = 1;
    sregs.cs.s = 1;
    sregs.cs.l = 0; // 32-bit mode, not 64-bit
    sregs.cs.g = 1;
    sregs.cs.avl = 0;
    sregs.cs.unusable = 0;

    // Flat 32-bit data segment shared by DS, ES, FS, GS, SS.
    struct kvm_segment data_seg = {
        .base = 0,
        .limit = 0xFFFFFFFF,
        .selector = 0x18,
        .type = 0x2, // data, read/write
        .present = 1,
        .dpl = 0,
        .db = 1,
        .s = 1,
        .l = 0,
        .g = 1,
        .avl = 0,
        .unusable = 0,
        .padding = 0,
    };
    sregs.ds = data_seg;
    sregs.es = data_seg;
    sregs.fs = data_seg;
    sregs.gs = data_seg;
    sregs.ss = data_seg;

    // Protected mode on; paging and write-protect off.
    sregs.cr0 = 0x1;
    sregs.cr4 = 0;
    sregs.efer = 0;

    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
    {
        perror("KVM_SET_SREGS");
        exit(1);
    }

    memset(&regs, 0, sizeof(regs));
    regs.rip = LINUX_KERNEL_ADDR; // protected-mode entry
    regs.rsi = LINUX_BOOT_PARAMS_ADDR;
    regs.rsp = 0x9000; // temp stack
    regs.rflags = 0x2; // reserved bit 1 always set

    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
    {
        perror("KVM_SET_REGS");
        exit(1);
    }
}