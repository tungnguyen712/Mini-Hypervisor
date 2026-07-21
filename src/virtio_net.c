#include "virtio_net.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

// --- virtio-mmio legacy (v1) register offsets ---------------------------
#define REG_MAGIC_VALUE 0x000
#define REG_VERSION 0x004
#define REG_DEVICE_ID 0x008
#define REG_VENDOR_ID 0x00c
#define REG_HOST_FEATURES 0x010
#define REG_HOST_FEATURES_SEL 0x014
#define REG_GUEST_FEATURES 0x020
#define REG_GUEST_FEATURES_SEL 0x024
#define REG_GUEST_PAGE_SIZE 0x028
#define REG_QUEUE_SEL 0x030
#define REG_QUEUE_NUM_MAX 0x034
#define REG_QUEUE_NUM 0x038
#define REG_QUEUE_ALIGN 0x03c
#define REG_QUEUE_PFN 0x040
#define REG_QUEUE_NOTIFY 0x050
#define REG_INTERRUPT_STATUS 0x060
#define REG_INTERRUPT_ACK 0x064
#define REG_STATUS 0x070
#define REG_CONFIG 0x100

#define VIRTIO_MAGIC 0x74726976u // "virt"
#define VIRTIO_ID_NET 1u
#define VIRTIO_VENDOR_ID 0x4d484b56u // "MHKV"
#define VIRTIO_NET_F_MAC (1u << 5)
#define VIRTIO_QUEUE_NUM_MAX 256u

#define VQ_RX 0
#define VQ_TX 1

#define VIRTIO_NET_HDR_LEN 10 // no offload features negotiated

static void fire_irq(struct virtio_net_device *dev)
{
    struct kvm_irq_level irq = {.irq = VIRTIO_NET_IRQ, .level = 1};
    ioctl(dev->vm_fd, KVM_IRQ_LINE, &irq);
    irq.level = 0;
    ioctl(dev->vm_fd, KVM_IRQ_LINE, &irq);
}

static void notify_used(struct virtio_net_device *dev)
{
    atomic_fetch_or(&dev->interrupt_status, 0x1);
    fire_irq(dev);
}

static int in_bounds(struct virtio_net_device *dev, uint64_t addr, uint64_t len)
{
    return addr <= dev->mem_size && len <= dev->mem_size - addr;
}

// read and write to guest RAM
static uint16_t gmem_read_u16(struct virtio_net_device *dev, uint64_t addr)
{
    uint16_t v = 0;
    if (in_bounds(dev, addr, sizeof(v)))
        memcpy(&v, (uint8_t *)dev->mem + addr, sizeof(v));
    return v;
}

static void gmem_write_u16(struct virtio_net_device *dev, uint64_t addr, uint16_t v)
{
    if (in_bounds(dev, addr, sizeof(v)))
        memcpy((uint8_t *)dev->mem + addr, &v, sizeof(v));
}

static uint64_t gmem_read_u64(struct virtio_net_device *dev, uint64_t addr)
{
    uint64_t v = 0;
    if (in_bounds(dev, addr, sizeof(v)))
        memcpy(&v, (uint8_t *)dev->mem + addr, sizeof(v));
    return v;
}

static void gmem_write_u32(struct virtio_net_device *dev, uint64_t addr, uint32_t v)
{
    if (in_bounds(dev, addr, sizeof(v)))
        memcpy((uint8_t *)dev->mem + addr, &v, sizeof(v));
}

// --- split-virtqueue layout (legacy: one contiguous region per queue) ----
// desc table -> avail ring -> pad to align -> used ring
// (no VIRTIO_RING_F_EVENT_IDX negotiated, so avail/used carry no trailing
// used_event/avail_event field)

struct vring_addrs
{
    uint64_t desc;
    uint64_t avail;
    uint64_t used;
};

// virtue queue structure
// Guest memory
// base
//  ↓
// [ Descriptor table ]
// [ Available ring   ]
// [ padding/alignment]
// [ Used ring        ]
static void queue_addrs(struct virtio_net_device *dev, int qidx, struct vring_addrs *out)
{
    struct virtio_net_queue *q = &dev->queues[qidx];
    uint64_t base = (uint64_t)q->pfn * (uint64_t)dev->guest_page_size;
    uint64_t desc = base;
    uint64_t avail = desc + 16ull * q->num;
    uint64_t avail_size = 4 + 2ull * q->num;
    uint32_t align = q->align ? q->align : 4096;
    uint64_t used = ((avail + avail_size + align - 1) / align) * align;
    out->desc = desc;   // descriptor table
    out->avail = avail; // available ring address
    out->used = used;   // used ring address
}

static uint16_t avail_idx(struct virtio_net_device *dev, struct vring_addrs *a)
{
    return gmem_read_u16(dev, a->avail + 2);
}

static uint16_t avail_ring_at(struct virtio_net_device *dev, struct vring_addrs *a, uint16_t i, uint32_t qnum)
{
    return gmem_read_u16(dev, a->avail + 4 + 2ull * (i % qnum));
}

struct desc
{
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

#define VRING_DESC_F_NEXT 1u
#define VRING_DESC_F_WRITE 2u

static struct desc read_desc(struct virtio_net_device *dev, uint64_t desc_table, uint16_t idx)
{
    uint64_t off = desc_table + 16ull * idx;
    struct desc d;
    d.addr = gmem_read_u64(dev, off);
    uint32_t len = 0;
    if (in_bounds(dev, off + 8, sizeof(len)))
        memcpy(&len, (uint8_t *)dev->mem + off + 8, sizeof(len));
    d.len = len;
    d.flags = gmem_read_u16(dev, off + 12);
    d.next = gmem_read_u16(dev, off + 14);
    return d;
}

static void used_push(struct virtio_net_device *dev, struct vring_addrs *a, uint32_t qnum,
                      uint16_t used_idx, uint16_t desc_id, uint32_t len)
{
    uint64_t elem = a->used + 4 + 8ull * (used_idx % qnum);
    gmem_write_u32(dev, elem, desc_id);
    gmem_write_u32(dev, elem + 4, len);
    gmem_write_u16(dev, a->used + 2, (uint16_t)(used_idx + 1));
}

// TX: guest -> host -> tap
static void process_tx(struct virtio_net_device *dev)
{
    // select queue 1 (TX)
    struct virtio_net_queue *q = &dev->queues[VQ_TX];
    if (q->num == 0)
        return;
    // calculate queue address
    struct vring_addrs a;
    queue_addrs(dev, VQ_TX, &a);
    // find newly submitted packets
    uint16_t new_idx = avail_idx(dev, &a);
    int sent_any = 0;

    while (q->last_avail_idx != new_idx)
    {
        // get the descriptor chain
        uint16_t head = avail_ring_at(dev, &a, q->last_avail_idx, q->num);
        struct iovec iov[16];
        int niov = 0;
        uint16_t cur = head;
        int skip = VIRTIO_NET_HDR_LEN;
        for (int hops = 0; hops < 16; hops++)
        {
            struct desc d = read_desc(dev, a.desc, cur);
            uint64_t addr = d.addr;
            uint32_t len = d.len;
            if (skip > 0)
            {
                uint32_t take = skip < (int)len ? (uint32_t)skip : len;
                addr += take;
                len -= take;
                skip -= (int)take;
            }
            if (len > 0 && niov < 16 && in_bounds(dev, addr, len))
            {
                iov[niov].iov_base = (uint8_t *)dev->mem + addr;
                iov[niov].iov_len = len;
                niov++;
            }
            if (!(d.flags & VRING_DESC_F_NEXT))
                break;
            cur = d.next;
        }

        if (niov > 0 && dev->tap_fd >= 0)
            (void)writev(dev->tap_fd, iov, niov);

        used_push(dev, &a, q->num, gmem_read_u16(dev, a.used + 2), head, 0);
        q->last_avail_idx++;
        sent_any = 1;
    }

    if (sent_any)
        notify_used(dev);
}

// RX: tap -> host -> guest

static int process_rx(struct virtio_net_device *dev, const uint8_t *frame, size_t frame_len)
{
    struct virtio_net_queue *q = &dev->queues[VQ_RX];
    if (q->num == 0)
        return 0;

    struct vring_addrs a;
    queue_addrs(dev, VQ_RX, &a);
    uint16_t new_idx = avail_idx(dev, &a);
    if (q->last_avail_idx == new_idx)
        return 0; // guest has posted no free buffer -> drop

    uint16_t head = avail_ring_at(dev, &a, q->last_avail_idx, q->num);
    struct desc d = read_desc(dev, a.desc, head);

    if (!(d.flags & VRING_DESC_F_WRITE) || d.len < VIRTIO_NET_HDR_LEN + frame_len ||
        !in_bounds(dev, d.addr, d.len))
    {
        q->last_avail_idx++;
        return 0;
    }

    uint8_t *dst = (uint8_t *)dev->mem + d.addr;
    memset(dst, 0, VIRTIO_NET_HDR_LEN);
    memcpy(dst + VIRTIO_NET_HDR_LEN, frame, frame_len);

    used_push(dev, &a, q->num, gmem_read_u16(dev, a.used + 2), head,
              (uint32_t)(VIRTIO_NET_HDR_LEN + frame_len));
    q->last_avail_idx++;
    notify_used(dev);
    return 1;
}

static void *rx_thread_main(void *arg)
{
    struct virtio_net_device *dev = (struct virtio_net_device *)arg;
    uint8_t buf[2048];

    while (!atomic_load(dev->stop_requested))
    {
        struct pollfd pfd = {.fd = dev->tap_fd, .events = POLLIN};
        int rc = poll(&pfd, 1, 100);
        if (rc <= 0)
            continue;
        if (!(pfd.revents & POLLIN))
            continue;

        ssize_t n = read(dev->tap_fd, buf, sizeof(buf));
        if (n <= 0)
            continue;

        process_rx(dev, buf, (size_t)n);
    }
    return NULL;
}

// register file

static void reset_device(struct virtio_net_device *dev)
{
    dev->guest_features_sel = 0;
    dev->host_features_sel = 0;
    dev->guest_features = 0;
    dev->queue_sel = 0;
    dev->status = 0;
    atomic_store(&dev->interrupt_status, 0);
    memset(dev->queues, 0, sizeof(dev->queues));
    dev->queues[VQ_RX].num_max = VIRTIO_QUEUE_NUM_MAX;
    dev->queues[VQ_TX].num_max = VIRTIO_QUEUE_NUM_MAX;
}

struct virtio_net_device *virtio_net_create(int vm_fd, int tap_fd, int vm_id,
                                            void *mem, size_t mem_size,
                                            atomic_int *stop_requested)
{
    struct virtio_net_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
    {
        perror("calloc virtio_net_device");
        return NULL;
    }
    // connect device to its VM
    dev->vm_fd = vm_fd;
    dev->tap_fd = tap_fd;
    dev->vm_id = vm_id;
    dev->mem = mem;
    dev->mem_size = mem_size;
    dev->stop_requested = stop_requested;

    // generate a MAC address for this VM
    tap_mac_for_vm(vm_id, dev->mac);
    dev->config_status[0] = 0x01;
    dev->config_status[1] = 0x00;

    reset_device(dev);

    // start RX thread
    if (pthread_create(&dev->rx_thread, NULL, rx_thread_main, dev) != 0)
    {
        perror("pthread_create virtio-net rx thread");
        free(dev);
        return NULL;
    }
    dev->rx_thread_valid = 1;
    return dev;
}

void virtio_net_destroy(struct virtio_net_device *dev)
{
    if (!dev)
        return;
    if (dev->rx_thread_valid)
        pthread_join(dev->rx_thread, NULL);
    free(dev);
}

void virtio_net_mmio_access(struct virtio_net_device *dev, uint64_t offset,
                            uint8_t *data, uint32_t len, int is_write)
{
    if (offset >= REG_CONFIG)
    {
        uint64_t cfg_off = offset - REG_CONFIG;
        for (uint32_t i = 0; i < len; i++)
        {
            uint64_t idx = cfg_off + i;
            if (idx < 6)
            {
                if (is_write)
                    dev->mac[idx] = data[i];
                else
                    data[i] = dev->mac[idx];
            }
            else if (idx < 8)
            {
                if (is_write)
                    dev->config_status[idx - 6] = data[i];
                else
                    data[i] = dev->config_status[idx - 6];
            }
            else if (!is_write)
            {
                data[i] = 0;
            }
        }
        return;
    }

    if (!is_write)
    {
        uint32_t val = 0;
        switch (offset)
        {
        case REG_MAGIC_VALUE:
            val = VIRTIO_MAGIC;
            break;
        case REG_VERSION:
            val = 1;
            break;
        case REG_DEVICE_ID:
            val = VIRTIO_ID_NET;
            break;
        case REG_VENDOR_ID:
            val = VIRTIO_VENDOR_ID;
            break;
        case REG_HOST_FEATURES:
            val = (dev->host_features_sel == 0) ? VIRTIO_NET_F_MAC : 0;
            break;
        case REG_QUEUE_NUM_MAX:
            val = dev->queues[dev->queue_sel & 1].num_max;
            break;
        case REG_QUEUE_PFN:
            val = dev->queues[dev->queue_sel & 1].pfn;
            break;
        case REG_INTERRUPT_STATUS:
            val = atomic_load(&dev->interrupt_status);
            break;
        case REG_STATUS:
            val = dev->status;
            break;
        default:
            val = 0;
            break;
        }
        uint32_t n = len < 4 ? len : 4;
        memcpy(data, &val, n);
        return;
    }

    // is_write
    uint32_t val = 0;
    uint32_t n = len < 4 ? len : 4;
    memcpy(&val, data, n);

    switch (offset)
    {
    case REG_HOST_FEATURES_SEL:
        dev->host_features_sel = val;
        break;
    case REG_GUEST_FEATURES:
        dev->guest_features = val;
        break;
    case REG_GUEST_FEATURES_SEL:
        dev->guest_features_sel = val;
        break;
    case REG_GUEST_PAGE_SIZE:
        dev->guest_page_size = val;
        break;
    case REG_QUEUE_SEL:
        dev->queue_sel = val & 1;
        break;
    case REG_QUEUE_NUM:
        dev->queues[dev->queue_sel & 1].num =
            val < VIRTIO_QUEUE_NUM_MAX ? val : VIRTIO_QUEUE_NUM_MAX;
        break;
    case REG_QUEUE_ALIGN:
        dev->queues[dev->queue_sel & 1].align = val;
        break;
    case REG_QUEUE_PFN:
        dev->queues[dev->queue_sel & 1].pfn = val;
        break;
    case REG_QUEUE_NOTIFY:
        if (val == VQ_TX)
            process_tx(dev);
        // val == VQ_RX: nothing to do here, the RX thread drives itself.
        break;
    case REG_INTERRUPT_ACK:
        atomic_fetch_and(&dev->interrupt_status, (unsigned char)~val);
        break;
    case REG_STATUS:
        dev->status = val;
        if (val == 0)
            reset_device(dev);
        break;
    default:
        break;
    }
}
