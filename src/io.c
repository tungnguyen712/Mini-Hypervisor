#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static device_bus bus = {.lock = PTHREAD_MUTEX_INITIALIZER};

void handle_io(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    // payload only handle 'out' I/O operations (guest write to port)
    // temporarily treating any 'in' flow as illegal (guest read from port)
    if (kvm_run->io.direction != KVM_EXIT_IO_OUT)
    {
        fprintf(stderr, "Unhandled I/O direction: %d\n", kvm_run->io.direction);
        exit(1);
    }
    if (kvm_run->io.port != 0x3f8) // only handle COM1 port
    {
        fprintf(stderr, "Unhandled I/O port: 0x%x\n", kvm_run->io.port);
        exit(1);
    }
    uint8_t *data = (uint8_t *)kvm_run + kvm_run->io.data_offset;
    pthread_mutex_lock(&bus.lock);
    for (uint32_t i = 0; i < kvm_run->io.count; i++)
    {
        if (kvm_run->io.size <= 1)
        {
            putchar(data[i]);
        }
    }
    fflush(stdout);
    pthread_mutex_unlock(&bus.lock);
}

void handle_mmio(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    pthread_mutex_lock(&bus.lock);
    if (kvm_run->mmio.is_write)
    {
        printf("KVM_EXIT_MMIO: write 0x%02x to guest phys 0x%llx (len %u)\n",
               kvm_run->mmio.data[0],
               (unsigned long long)kvm_run->mmio.phys_addr,
               kvm_run->mmio.len);
    }
    else
    {
        printf("KVM_EXIT_MMIO: read from guest phys 0x%llx (len %u)\n",
               (unsigned long long)kvm_run->mmio.phys_addr,
               kvm_run->mmio.len);
        // no real device backs this address yet; return zero so the guest
        // doesn't hang waiting on a response from a device that doesn't exist
        memset(kvm_run->mmio.data, 0, kvm_run->mmio.len);
    }
    pthread_mutex_unlock(&bus.lock);
}