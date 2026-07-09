#include "vm.h"
#include "vcpu.h"
#include "guest_code.h"

#define GUEST_MEM_SIZE (1 << 20) // 1 MB placeholder for guest memory size

int main()
{
    struct vm vm;
    struct vcpu vcpu;

    vm_init(&vm, GUEST_MEM_SIZE);
    load_payload(&vm, guest_code, sizeof(guest_code));
    vcpu_init(&vm, &vcpu);
    vcpu_setup_regs(&vcpu);
    vcpu_run(&vcpu);

    vcpu_cleanup(&vcpu);
    vm_cleanup(&vm);
    return 0;
}