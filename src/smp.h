#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "vcpu.h"

struct vcpu_thread_arg
{
    struct vm *vm;
    struct vcpu *vcpu;
    unsigned long vcpu_id;
    uint64_t rip;
};

void *vcpu_thread_main(void *arg);

#endif // SMP_H