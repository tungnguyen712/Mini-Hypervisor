#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

// Guest Linux
//    │
//    │ Virtio network driver
//    ▼
// Virtio-net implemented by this file
//    │
//    │ Ethernet frames
//    ▼
// TAP file descriptor
//    │
//    ▼
// Host Linux networking stack → NAT → Internet

#define VIRTIO_NET_MMIO_BASE 0xd0000000ULL
#define VIRTIO_NET_MMIO_SIZE 0x200u
#define VIRTIO_NET_IRQ 9

// store host-side state for virtio queues, one for RX and one for TX
struct virtio_net_queue
{
    uint32_t num_max;
    uint32_t num;
    uint32_t align;
    uint32_t pfn;
    uint16_t last_avail_idx;
};
// store state of a single virtio-net device instance, one per VM
struct virtio_net_device
{
    int vm_fd;
    int tap_fd;
    int vm_id;
    // guest RAM
    void *mem;
    size_t mem_size;
    // feature negotiation and queue configuration
    uint32_t guest_features_sel;
    uint32_t host_features_sel;
    uint32_t guest_features;
    uint32_t guest_page_size;
    uint32_t queue_sel;
    uint32_t status;
    atomic_uchar interrupt_status;

    struct virtio_net_queue queues[2]; // 0 = RX, 1 = TX

    uint8_t mac[6];
    uint8_t config_status[2];

    atomic_int *stop_requested;
    pthread_t rx_thread;
    int rx_thread_valid;
};

// - the device translates guest-physical virtqueue addresses directly out of
// it, the same pattern vm.c already uses for boot_params/cmdline/kernel
// loading. Spawns the RX-polling thread internally.
struct virtio_net_device *virtio_net_create(int vm_fd, int tap_fd, int vm_id,
                                            void *mem, size_t mem_size,
                                            atomic_int *stop_requested);
// join RX thread and free device struct
void virtio_net_destroy(struct virtio_net_device *dev);

// handle guests reads/writes to virtual NIC's range
void virtio_net_mmio_access(struct virtio_net_device *dev, uint64_t offset,
                            uint8_t *data, uint32_t len, int is_write);

#endif // VIRTIO_NET_H
