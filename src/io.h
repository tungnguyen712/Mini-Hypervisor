#ifndef IO_H
#define IO_H

#include "vcpu.h"
#include <pthread.h>
#include <stdint.h>

#define COM1_RX_BUF_SIZE 256

struct com1_device
{
    pthread_mutex_t rx_mutex;
    pthread_mutex_t tx_mutex;
    uint8_t rx_buf[COM1_RX_BUF_SIZE];
    int rx_head, rx_tail;
    uint8_t reg_ier, reg_lcr, reg_mcr, reg_scr;
    int tx_thre_pending;
    int vm_fd;
    int log_fd;
};

// Allocates and zero-init a com1_device, then opens log_path
// to capture guest serial output
struct com1_device *com1_create(int vm_fd, const char *log_path);
void com1_destroy(struct com1_device *dev);

// handle port I/O operations
void handle_io(struct vcpu *vcpu);
// handle memory-mapped I/O operations (guest touch guest physical address space not backed by RAM)
void handle_mmio(struct vcpu *vcpu);

// COM1 serial receive interface
int com1_rx_avail(struct com1_device *dev);
uint8_t com1_ier(struct com1_device *dev);

#endif // IO_H