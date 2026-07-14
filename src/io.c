#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

static pthread_mutex_t mmio_log_lock = PTHREAD_MUTEX_INITIALIZER;

struct com1_device *com1_create(int vm_fd, const char *log_path)
{
    struct com1_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
    {
        perror("calloc com1_device");
        return NULL;
    }

    pthread_mutex_init(&dev->rx_mutex, NULL);
    pthread_mutex_init(&dev->tx_mutex, NULL);
    dev->vm_fd = vm_fd;

    dev->log_fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (dev->log_fd < 0)
    {
        perror(log_path);
        fprintf(stderr, "com1_create: continuing without a serial log for %s\n", log_path);
    }

    return dev;
}

void com1_destroy(struct com1_device *dev)
{
    if (!dev)
        return;
    if (dev->log_fd >= 0)
        close(dev->log_fd);
    pthread_mutex_destroy(&dev->rx_mutex);
    pthread_mutex_destroy(&dev->tx_mutex);
    free(dev);
}

// Pulse IRQ 4. Safe to call from any thread: with an in-kernel irqchip,
// KVM_IRQ_LINE wakes a vCPU that's blocked inside the kernel on HLT.
static void com1_fire_irq(struct com1_device *dev)
{
    struct kvm_irq_level irq = {.irq = 4, .level = 1};
    ioctl(dev->vm_fd, KVM_IRQ_LINE, &irq);
    irq.level = 0;
    ioctl(dev->vm_fd, KVM_IRQ_LINE, &irq);
}

int com1_rx_avail(struct com1_device *dev)
{
    pthread_mutex_lock(&dev->rx_mutex);
    // if head == tail, buffer is empty, or can't input more
    int avail = (dev->rx_head != dev->rx_tail);
    pthread_mutex_unlock(&dev->rx_mutex);
    return avail;
}

static uint8_t rx_get_locked(struct com1_device *dev) // assume caller must hold dev->rx_mutex
{
    uint8_t byte = dev->rx_buf[dev->rx_tail];
    dev->rx_tail = (dev->rx_tail + 1) % COM1_RX_BUF_SIZE;
    return byte;
}

uint8_t com1_ier(struct com1_device *dev)
{
    pthread_mutex_lock(&dev->rx_mutex);
    uint8_t v = dev->reg_ier;
    pthread_mutex_unlock(&dev->rx_mutex);
    return v;
}

// Handle an IN (guest reads from a port) exit
static void handle_io_in(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    struct com1_device *com1 = vcpu->vm->com1;
    uint16_t port = kvm_run->io.port;
    uint8_t *data = (uint8_t *)kvm_run + kvm_run->io.data_offset;
    uint32_t size = kvm_run->io.size;
    uint32_t count = kvm_run->io.count;

    // Safe default for all unrecognised ports: 0xFF.
    memset(data, 0xFF, size * count);

    if (port >= 0x3f8 && port <= 0x3ff)
    {
        // COM1 (16550 UART)
        switch (port - 0x3f8)
        {
        case 0: // RBR: receive data register
            pthread_mutex_lock(&com1->rx_mutex);
            if (com1->rx_head != com1->rx_tail)
                memset(data, rx_get_locked(com1), count);
            else
                memset(data, 0x00, size * count);
            pthread_mutex_unlock(&com1->rx_mutex);
            break;
        case 2:
        {
            pthread_mutex_lock(&com1->rx_mutex);
            uint8_t iir;
            if (com1->rx_head != com1->rx_tail)
                iir = 0x04;
            else if (com1->tx_thre_pending)
            {
                iir = 0x02;
                com1->tx_thre_pending = 0;
            }
            else
                iir = 0x01;
            pthread_mutex_unlock(&com1->rx_mutex);
            memset(data, iir, count);
        }
        break;
        case 6:
            memset(data, 0xB0, count);
            break;
        case 5:
        {
            pthread_mutex_lock(&com1->rx_mutex);
            uint8_t dr = (com1->rx_head != com1->rx_tail) ? 0x01 : 0x00;
            pthread_mutex_unlock(&com1->rx_mutex);
            memset(data, (uint8_t)(0x60 | dr), count);
            break;
        }
        case 1: // IER: Interrupt Enable Register
            memset(data, com1->reg_ier, count);
            break;
        case 3: // LCR: Line Control Register
            memset(data, com1->reg_lcr, count);
            break;
        case 4: // MCR: Modem Control Register
            memset(data, com1->reg_mcr, count);
            break;
        case 7: // SCR: Scratch Register
            memset(data, com1->reg_scr, count);
            break;
        default:
            memset(data, 0x00, size * count);
            break;
        }
        return;
    }

    if (port == 0x92)
    {
        // Fast A20 gate: bit 1 = A20 line already enabled, nothing to do
        data[0] = 0x02;
        return;
    }

    if (port == 0x64)
    {
        // PS/2 controller status: both input and output buffers empty
        data[0] = 0x00;
        return;
    }

    if (port == 0x20 || port == 0x21 || port == 0xa0 || port == 0xa1)
    {
        // 8259A PIC: return 0x00 so ISR/IRR show no interrupts in service
        memset(data, 0x00, size * count);
        return;
    }

    // All other ports 0xFF already set.
}

// Handle an OUT (guest writes to a port) exit.
static void handle_io_out(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    struct com1_device *com1 = vcpu->vm->com1;
    uint8_t *data = (uint8_t *)kvm_run + kvm_run->io.data_offset;

    if (kvm_run->io.port == 0x3f8)
    {
        // COM1 data register: capture each byte to this VM's serial log.
        pthread_mutex_lock(&com1->tx_mutex);
        if (com1->log_fd >= 0)
            write(com1->log_fd, data, kvm_run->io.count);
        pthread_mutex_unlock(&com1->tx_mutex);

        pthread_mutex_lock(&com1->rx_mutex);
        int thri_enabled = (com1->reg_ier & 0x02) != 0;
        if (thri_enabled)
            com1->tx_thre_pending = 1;
        pthread_mutex_unlock(&com1->rx_mutex);
        if (thri_enabled)
            com1_fire_irq(com1);
        return;
    }

    if (kvm_run->io.port == 0x3f9)
    { // IER: only the low 4 bits are defined on a real 16550
        pthread_mutex_lock(&com1->rx_mutex);
        com1->reg_ier = data[0] & 0x0F;
        int thri_enabled = (com1->reg_ier & 0x02) != 0;
        if (thri_enabled)
            com1->tx_thre_pending = 1; // THR is always empty here, so signal immediately
        pthread_mutex_unlock(&com1->rx_mutex);
        if (thri_enabled)
            com1_fire_irq(com1);
        return;
    }
    if (kvm_run->io.port == 0x3fb)
    { // LCR
        com1->reg_lcr = data[0];
        return;
    }
    if (kvm_run->io.port == 0x3fc)
    { // MCR
        com1->reg_mcr = data[0];
        return;
    }
    if (kvm_run->io.port == 0x3ff)
    { // SCR
        com1->reg_scr = data[0];
        return;
    }

    // All other OUT ports silently accept and discard.
}

void handle_io(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    if (kvm_run->io.direction == KVM_EXIT_IO_IN)
        handle_io_in(vcpu);
    else
        handle_io_out(vcpu);
}

void handle_mmio(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;

    if (kvm_run->mmio.phys_addr >= 0xfee00000ULL &&
        kvm_run->mmio.phys_addr <= 0xfee00fffULL)
    {
        if (!kvm_run->mmio.is_write)
            memset(kvm_run->mmio.data, 0, kvm_run->mmio.len);
        return;
    }

    pthread_mutex_lock(&mmio_log_lock);
    if (kvm_run->mmio.is_write)
    {
        printf("[vm %d] KVM_EXIT_MMIO: write 0x%02x to guest phys 0x%llx (len %u)\n",
               vcpu->vm->id,
               kvm_run->mmio.data[0],
               (unsigned long long)kvm_run->mmio.phys_addr,
               kvm_run->mmio.len);
    }
    else
    {
        printf("[vm %d] KVM_EXIT_MMIO: read from guest phys 0x%llx (len %u)\n",
               vcpu->vm->id,
               (unsigned long long)kvm_run->mmio.phys_addr,
               kvm_run->mmio.len);
        // no real device backs this address yet; return zero so the guest
        // doesn't hang waiting on a response from a device that doesn't exist
        memset(kvm_run->mmio.data, 0, kvm_run->mmio.len);
    }
    pthread_mutex_unlock(&mmio_log_lock);
}