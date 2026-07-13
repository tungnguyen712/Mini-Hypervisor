#include "smp.h"

void *vcpu_thread_main(void *arg)
{
    struct vcpu_thread_arg *a = (struct vcpu_thread_arg *)arg;
    vcpu_init(a->vm, a->vcpu, a->vcpu_id);
    if (a->use_linux_entry)
        vcpu_setup_linux32_entry(a->vcpu);
    else
        vcpu_setup_regs(a->vcpu, a->rip);
    vcpu_run(a->vcpu);
    vcpu_get_sregs(a->vcpu, &a->sregs);
    vcpu_cleanup(a->vcpu);
    return NULL;
}