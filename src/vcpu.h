#ifndef VCPU_H
#define VCPU_H

#include "vm.h"
#include <linux/kvm.h>
#include <stdint.h>

struct vcpu
{
    int fd;
    int vm_fd;
    struct kvm_run *kvm_run;
    int kvm_run_size;
    struct vm *vm;
};

// All functions below return 0 on success, -1 on failure.
int vcpu_setup_regs(struct vcpu *vcpu, uint64_t rip);
int vcpu_init(struct vm *vm, struct vcpu *vcpu, unsigned long vcpu_id);
int vcpu_get_sregs(struct vcpu *vcpu, struct kvm_sregs *sregs);

// Runs the vCPU until it halts/errors/triple-faults, or vm->stop_requested is
// set. Returns 1 if stopped by request, -1 on error
int vcpu_run(struct vcpu *vcpu);
void vcpu_cleanup(struct vcpu *vcpu);

// Configure a VCPU to enter the Linux kernel at the 32-bit protected-mode
int vcpu_setup_linux32_entry(struct vcpu *vcpu);

#endif // VCPU_H