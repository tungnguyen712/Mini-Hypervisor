#include "vm.h"
#include "vcpu.h"
#include "smp.h"
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

// only back 640KB conventional memory region as real RAM
// anything from here up to real-mode's 1MB will trigger KVM_EXIT_MMIO
#define GUEST_MEM_SIZE 0x4000

#define VCPU0_RIP 0x0000
#define VCPU1_RIP 0x1000
#define COUNTER_ADDR 0x2000
#define LOOP_COUNT 1000

int main()
{
    struct vm vm;
    struct vcpu vcpu0, vcpu1;

    vm_init(&vm, GUEST_MEM_SIZE);
    load_payload_from_file_at(&vm, "guest/payloads/vcpu_counter.bin", VCPU0_RIP);
    load_payload_from_file_at(&vm, "guest/payloads/vcpu_counter.bin", VCPU1_RIP);

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

    pthread_t thread0, thread1;
    if (pthread_create(&thread0, NULL, vcpu_thread_main, &arg0) != 0)
    {
        perror("pthread_create vcpu0");
    }
    if (pthread_create(&thread1, NULL, vcpu_thread_main, &arg1) != 0)
    {
        perror("pthread_create vcpu1");
    }

    pthread_join(thread0, NULL);
    pthread_join(thread1, NULL);

    uint16_t *counter = (uint16_t *)((unsigned char *)vm.mem + COUNTER_ADDR);
    uint16_t expected = 2 * LOOP_COUNT;
    printf("Shared counter = %u (expected %u)\n", *counter, expected);

    vm_cleanup(&vm);
    return 0;
}