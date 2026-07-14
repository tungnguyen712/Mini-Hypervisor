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
    int use_linux_entry;
    struct kvm_sregs sregs;
    int result; // set by vcpu_thread_main: 1 = stopped by request, -1 = error
};

void *vcpu_thread_main(void *arg);

#endif // SMP_H