#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void handle_io(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    // our payload only handle 'out' I/O operations (guest write to port)
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
    for (uint32_t i = 0; i < kvm_run->io.count; i++)
    {
        if (kvm_run->io.size <= 1)
        {
            putchar(data[i]);
        }
    }
    fflush(stdout);
}