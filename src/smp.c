#include "smp.h"

void *vcpu_thread_main(void *arg)
{
    struct vcpu_thread_arg *a = (struct vcpu_thread_arg *)arg;

    if (vcpu_init(a->vm, a->vcpu, a->vcpu_id) != 0)
    {
        a->result = -1;
        return NULL;
    }

    int setup_rc = a->use_linux_entry
        ? vcpu_setup_linux32_entry(a->vcpu)
        : vcpu_setup_regs(a->vcpu, a->rip);
    if (setup_rc != 0)
    {
        a->result = -1;
        vcpu_cleanup(a->vcpu);
        return NULL;
    }

    a->result = vcpu_run(a->vcpu); // 1 = stopped by request, -1 = error
    vcpu_get_sregs(a->vcpu, &a->sregs);
    vcpu_cleanup(a->vcpu);
    return NULL;
}