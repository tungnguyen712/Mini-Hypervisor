#ifndef IO_H
#define IO_H

#include "vcpu.h"

void handle_io(struct vcpu *vcpu); // handle port I/O operations
void handle_mmio(struct vcpu *vcpu); // handle memory-mapped I/O operations (guest touch non-allocated memory)

#endif // IO_H