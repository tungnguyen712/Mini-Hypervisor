#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "vcpu.h"
#include "vm.h"

struct vcpu_thread_arg
{
    struct vm *vm;
    struct vcpu *vcpu;
    unsigned long vcpu_id;
    uint64_t rip;
    struct kvm_sregs sregs;
};

void *vcpu_thread_main(void *arg);

#endif // SMP_H