#ifndef IO_H
#define IO_H

#include "vcpu.h"
#include <pthread.h>
#include <stdint.h>

typedef struct
{
    pthread_mutex_t lock;
} device_bus;

void handle_io(struct vcpu *vcpu);   // handle port I/O operations
void handle_mmio(struct vcpu *vcpu); // handle memory-mapped I/O operations (guest touch non-allocated memory)

// COM1 serial receive interface
int com1_rx_avail(void);
void com1_start_input_thread(void);
uint8_t com1_ier(void); // current value of the Interrupt Enable Register
void com1_set_vm_fd(int fd);

#endif // IO_H