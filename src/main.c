#include "vm.h"
#include "vcpu.h"
#include <stdio.h>
#include <stdint.h>

// only back 640KB conventional memory region as real RAM
// anything from here up to real-mode's 1MB will trigger KVM_EXIT_MMIO
#define GUEST_MEM_SIZE 0xA0000

#define NEAR_ADDR 0x200
#define NEAR_VAL 0xAA
#define FAR_ADDR 0x9FFF0
#define FAR_VAL 0xBB

int main()
{
    struct vm vm;
    struct vcpu vcpu;

    vm_init(&vm, GUEST_MEM_SIZE);
    load_payload_from_file(&vm, "guest/payloads/payload1.bin");
    vcpu_init(&vm, &vcpu);
    vcpu_setup_regs(&vcpu);
    vcpu_run(&vcpu);
    uint8_t *mem = (uint8_t *)vm.mem;
    int near_ok = (mem[NEAR_ADDR] == NEAR_VAL);
    int far_ok = (mem[FAR_ADDR] == FAR_VAL);

    if (!near_ok || !far_ok)
    {
        fprintf(stderr, "  near 0x%x = 0x%02x (want 0x%02x)\n", NEAR_ADDR, mem[NEAR_ADDR], NEAR_VAL);
        fprintf(stderr, "  far  0x%x = 0x%02x (want 0x%02x)\n", FAR_ADDR, mem[FAR_ADDR], FAR_VAL);
    }

    vcpu_cleanup(&vcpu);
    vm_cleanup(&vm);
    return 0;
}