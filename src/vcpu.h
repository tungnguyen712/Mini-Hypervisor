#ifndef VCPU_H
#define VCPU_H

#include "vm.h"
#include <linux/kvm.h>
#include <stdint.h>

struct vcpu
{
    int fd;
    struct kvm_run *kvm_run;
    int kvm_run_size;
};

void vcpu_setup_regs(struct vcpu *vcpu, uint64_t rip);
void vcpu_init(struct vm *vm, struct vcpu *vcpu, unsigned long vcpu_id);
int vcpu_run(struct vcpu *vcpu);
void vcpu_cleanup(struct vcpu *vcpu);

#endif // VCPU_H