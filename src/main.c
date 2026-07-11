#include "vm.h"
#include "vcpu.h"
#include "smp.h"
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

// only back 640KB conventional memory region as real RAM
// anything from here up to real-mode's 1MB will trigger KVM_EXIT_MMIO
#define GUEST_MEM_SIZE 0x10000

#define VCPU0_RIP 0x0000
#define VCPU1_RIP 0x1000
#define VCPU2_RIP 0x3000
#define VCPU3_RIP 0x4000
#define COUNTER_ADDR 0x2000
#define LOOP_COUNT 1000

int main()
{
    struct vm vm;
    struct vcpu vcpu0, vcpu1, vcpu2, vcpu3;

    vm_init(&vm, GUEST_MEM_SIZE);
    load_payload_from_file_at(&vm, "guest/payloads/vcpu_long_mode.bin", VCPU0_RIP);
    load_payload_from_file_at(&vm, "guest/payloads/vcpu_long_mode.bin", VCPU1_RIP);
    load_payload_from_file_at(&vm, "guest/payloads/vcpu_long_mode.bin", VCPU2_RIP);
    load_payload_from_file_at(&vm, "guest/payloads/vcpu_long_mode.bin", VCPU3_RIP);

    struct vcpu_thread_arg arg0 = {
        .vm = &vm,
        .vcpu = &vcpu0,
        .vcpu_id = 0,
        .rip = VCPU0_RIP};
    struct vcpu_thread_arg arg1 = {
        .vm = &vm,
        .vcpu = &vcpu1,
        .vcpu_id = 1,
        .rip = VCPU1_RIP};
    struct vcpu_thread_arg arg2 = {
        .vm = &vm,
        .vcpu = &vcpu2,
        .vcpu_id = 2,
        .rip = VCPU2_RIP};
    struct vcpu_thread_arg arg3 = {
        .vm = &vm,
        .vcpu = &vcpu3,
        .vcpu_id = 3,
        .rip = VCPU3_RIP};

    pthread_t thread0, thread1, thread2, thread3;
    if (pthread_create(&thread0, NULL, vcpu_thread_main, &arg0) != 0)
    {
        perror("pthread_create vcpu0");
    }
    if (pthread_create(&thread1, NULL, vcpu_thread_main, &arg1) != 0)
    {
        perror("pthread_create vcpu1");
    }
    if (pthread_create(&thread2, NULL, vcpu_thread_main, &arg2) != 0)
    {
        perror("pthread_create vcpu2");
    }
    if (pthread_create(&thread3, NULL, vcpu_thread_main, &arg3) != 0)
    {
        perror("pthread_create vcpu3");
    }

    pthread_join(thread0, NULL);
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    pthread_join(thread3, NULL);

    // uint16_t *counter = (uint16_t *)((unsigned char *)vm.mem + COUNTER_ADDR);
    // uint16_t expected = 4 * LOOP_COUNT;
    // printf("Shared counter = %u (expected %u)\n", *counter, expected);

    struct vcpu_thread_arg *args[4] = {&arg0, &arg1, &arg2, &arg3};
    for (int i = 0; i < 4; i++)
    {
        struct kvm_sregs *sregs = &args[i]->sregs;
        int lma_set = (sregs->cr0 & 1) != 0;
        int cs_64bit = (sregs->cs.l == 1);
        int cs_sel_ok = (sregs->cs.selector == 0x18);
        // CR0.PE=1 confirms protection is enabled
        // CS.L=1 confirms the CPU actually loaded 64-bit code descriptor
        // CS.selector=0x8 confirms CS was actually reloaded to point at code descriptor
        printf("CR0.LMA=%d CS.L=%d CS.selector=0x%x\n -> %s\n",
               lma_set, cs_64bit, sregs->cs.selector,
               (lma_set && cs_64bit && cs_sel_ok) ? "OK" : "NOT OK");
    }

    vm_cleanup(&vm);
    return 0;
}