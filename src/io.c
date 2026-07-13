#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

static device_bus bus = {.lock = PTHREAD_MUTEX_INITIALIZER};

// buffer between background thread reading stdin and guest vCPU thread
#define RX_BUF_SIZE 256
static uint8_t rx_buf[RX_BUF_SIZE];
static int rx_head = 0, rx_tail = 0;
static pthread_mutex_t rx_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint8_t reg_ier = 0, reg_lcr = 0, reg_mcr = 0, reg_scr = 0;

static int tx_thre_pending = 0;

static int com1_vm_fd = -1;

void com1_set_vm_fd(int fd)
{
    com1_vm_fd = fd;
}

// Pulse IRQ 4. Safe to call from any thread: with an in-kernel irqchip,
// KVM_IRQ_LINE wakes a vCPU that's blocked inside the kernel on HLT.
static void com1_fire_irq(void)
{
    if (com1_vm_fd < 0)
        return;
    struct kvm_irq_level irq = {.irq = 4, .level = 1};
    ioctl(com1_vm_fd, KVM_IRQ_LINE, &irq);
    irq.level = 0;
    ioctl(com1_vm_fd, KVM_IRQ_LINE, &irq);
}

static void rx_put(uint8_t byte)
{
    pthread_mutex_lock(&rx_mutex);
    int next = (rx_head + 1) % RX_BUF_SIZE;
    int should_fire = 0;
    if (next != rx_tail)
    { // drop bytes if full
        rx_buf[rx_head] = byte;
        rx_head = next;
        should_fire = (reg_ier & 0x01) != 0; // IER.RDI: receive interrupts enabled
    }
    pthread_mutex_unlock(&rx_mutex);
    if (should_fire)
        com1_fire_irq();
}

int com1_rx_avail(void)
{
    pthread_mutex_lock(&rx_mutex);
    // if head == tail, buffer is empty, or can't input more
    int avail = (rx_head != rx_tail);
    pthread_mutex_unlock(&rx_mutex);
    return avail;
}

static uint8_t rx_get_locked(void) // assume caller must hold rx_mutex
{
    uint8_t byte = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return byte;
}

uint8_t com1_ier(void)
{
    pthread_mutex_lock(&rx_mutex);
    uint8_t v = reg_ier;
    pthread_mutex_unlock(&rx_mutex);
    return v;
}

static struct termios saved_termios;

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
}

static void *stdin_reader_thread(void *arg)
{
    (void)arg;
    uint8_t byte;
    while (read(STDIN_FILENO, &byte, 1) == 1)
        rx_put(byte);
    return NULL;
}

void com1_start_input_thread(void)
{
    // Switch stdin to raw mode
    if (tcgetattr(STDIN_FILENO, &saved_termios) == 0)
    {
        struct termios raw = saved_termios;
        raw.c_lflag &= ~(uint32_t)(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(uint32_t)(IXON | ICRNL | BRKINT);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        atexit(restore_terminal);
    }
    pthread_t tid;
    pthread_create(&tid, NULL, stdin_reader_thread, NULL);
    pthread_detach(tid);
}

// Handle an IN (guest reads from a port) exit
static void handle_io_in(struct kvm_run *kvm_run)
{
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
            pthread_mutex_lock(&rx_mutex);
            if (rx_head != rx_tail)
                memset(data, rx_get_locked(), count);
            else
                memset(data, 0x00, size * count);
            pthread_mutex_unlock(&rx_mutex);
            break;
        case 2:
        {
            pthread_mutex_lock(&rx_mutex);
            uint8_t iir;
            if (rx_head != rx_tail)
                iir = 0x04;
            else if (tx_thre_pending)
            {
                iir = 0x02;
                tx_thre_pending = 0;
            }
            else
                iir = 0x01;
            pthread_mutex_unlock(&rx_mutex);
            memset(data, iir, count);
        }
        break;
        case 6:
            memset(data, 0xB0, count);
            break;
        case 5:
        {
            pthread_mutex_lock(&rx_mutex);
            uint8_t dr = (rx_head != rx_tail) ? 0x01 : 0x00;
            pthread_mutex_unlock(&rx_mutex);
            memset(data, (uint8_t)(0x60 | dr), count);
            break;
        }
        case 1: // IER: Interrupt Enable Register
            memset(data, reg_ier, count);
            break;
        case 3: // LCR: Line Control Register
            memset(data, reg_lcr, count);
            break;
        case 4: // MCR: Modem Control Register
            memset(data, reg_mcr, count);
            break;
        case 7: // SCR: Scratch Register
            memset(data, reg_scr, count);
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
static void handle_io_out(struct kvm_run *kvm_run)
{
    uint8_t *data = (uint8_t *)kvm_run + kvm_run->io.data_offset;

    if (kvm_run->io.port == 0x3f8)
    {
        // COM1 data register: relay each byte to stdout as the serial console
        pthread_mutex_lock(&bus.lock);
        for (uint32_t i = 0; i < kvm_run->io.count; i++)
            putchar(data[i]);
        fflush(stdout);
        pthread_mutex_unlock(&bus.lock);

        // Transmission is instantaneous in this emulation, so the THR is
        // immediately empty again. If the guest has THRI enabled, tell it
        // so it can send the next byte (see tx_thre_pending above).
        pthread_mutex_lock(&rx_mutex);
        int thri_enabled = (reg_ier & 0x02) != 0;
        if (thri_enabled)
            tx_thre_pending = 1;
        pthread_mutex_unlock(&rx_mutex);
        if (thri_enabled)
            com1_fire_irq();
        return;
    }

    if (kvm_run->io.port == 0x3f9)
    { // IER: only the low 4 bits are defined on a real 16550
        pthread_mutex_lock(&rx_mutex);
        reg_ier = data[0] & 0x0F;
        int thri_enabled = (reg_ier & 0x02) != 0;
        if (thri_enabled)
            tx_thre_pending = 1; // THR is always empty here, so signal immediately
        pthread_mutex_unlock(&rx_mutex);
        if (thri_enabled)
            com1_fire_irq();
        return;
    }
    if (kvm_run->io.port == 0x3fb)
    { // LCR
        reg_lcr = data[0];
        return;
    }
    if (kvm_run->io.port == 0x3fc)
    { // MCR
        reg_mcr = data[0];
        return;
    }
    if (kvm_run->io.port == 0x3ff)
    { // SCR
        reg_scr = data[0];
        return;
    }

    // All other OUT ports silently accept and discard.
}

void handle_io(struct vcpu *vcpu)
{
    struct kvm_run *kvm_run = vcpu->kvm_run;
    if (kvm_run->io.direction == KVM_EXIT_IO_IN)
        handle_io_in(kvm_run);
    else
        handle_io_out(kvm_run);
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