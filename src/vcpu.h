#ifndef VCPU_H
#define VCPU_H

#include "vm.h"
#include <linux/kvm.h>

struct vcpu
{
    int fd;
    struct kvm_run *kvm_run;
    int kvm_run_size;
};

void vcpu_setup_regs(struct vcpu *vcpu);
void vcpu_init(struct vm *vm, struct vcpu *vcpu);
int vcpu_run(struct vcpu *vcpu);
void vcpu_cleanup(struct vcpu *vcpu);

#endif // VCPU_H