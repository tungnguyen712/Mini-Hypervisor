#include "registry.h"
#include "vcpu.h"
#include "smp.h"
#include "io.h"
#include "tap.h"
#include "virtio_net.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>

#define VM_MAX_VCPUS 1

// start and wait for all vcpu threads to finish
static void *supervisor_main(void *arg)
{
    struct vm_slot *slot = (struct vm_slot *)arg;
    struct vm *vm = &slot->vm;

    // prepare vcpu data
    struct vcpu vcpus[VM_MAX_VCPUS];
    pthread_t threads[VM_MAX_VCPUS];
    struct vcpu_thread_arg targs[VM_MAX_VCPUS];
    unsigned started = 0;

    // start each vcpu thread
    for (; started < VM_MAX_VCPUS; started++)
    {
        targs[started] = (struct vcpu_thread_arg){
            .vm = vm,
            .vcpu = &vcpus[started],
            .vcpu_id = started,
            .rip = 0,
            .use_linux_entry = 1,
        };
        if (pthread_create(&threads[started], NULL, vcpu_thread_main, &targs[started]) != 0)
            break;
        if (started == 0)
        {
            slot->vcpu_tid = threads[0];
            slot->vcpu_tid_valid = 1;
        }
    }

    // wait for all vcpu threads to finish
    int any_error = (started == 0);
    for (unsigned i = 0; i < started; i++)
    {
        // wait for this vcpu thread to finish
        pthread_join(threads[i], NULL);
        if (targs[i].result == -1)
            any_error = 1;
    }

    pthread_mutex_lock(&slot->state_mutex);
    slot->state = VM_STATE_STOPPED;
    slot->last_exit_ok = !any_error;
    if (any_error)
        snprintf(slot->last_error, sizeof(slot->last_error),
                 "one or more vcpu threads exited with an error");
    pthread_mutex_unlock(&slot->state_mutex);
    return NULL;
}

void registry_init(struct registry *reg, const char *log_dir)
{
    memset(reg, 0, sizeof(*reg));
    pthread_mutex_init(&reg->lock, NULL);
    for (int i = 0; i < REGISTRY_MAX_VMS; i++)
        pthread_mutex_init(&reg->slots[i].state_mutex, NULL);
    reg->next_id = 1;
    snprintf(reg->log_dir, sizeof(reg->log_dir), "%s", log_dir);
}

static int find_ready_slot_by_id_locked(struct registry *reg, int id)
{
    for (int i = 0; i < REGISTRY_MAX_VMS; i++)
    {
        if (reg->slots[i].in_use && reg->slots[i].ready && reg->slots[i].id == id)
            return i;
    }
    return -1;
}

int registry_create_vm(struct registry *reg, const struct vm_config *cfg,
                       char *err_buf, size_t err_buf_len)
{
    pthread_mutex_lock(&reg->lock);
    // find and reserve 1st empty slot
    int idx = -1;
    for (int i = 0; i < REGISTRY_MAX_VMS; i++)
    {
        if (!reg->slots[i].in_use)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        snprintf(err_buf, err_buf_len, "registry full");
        return -1;
    }

    // init selected slot
    struct vm_slot *slot = &reg->slots[idx];
    memset(&slot->vm, 0, sizeof(slot->vm));
    slot->config = *cfg;
    slot->id = reg->next_id++;
    slot->state = VM_STATE_STOPPED;
    slot->last_exit_ok = 1;
    slot->last_error[0] = '\0';
    slot->ready = 0;  // client can't see or use this slot until setup completes
    slot->in_use = 1; // block other CREATEs from picking this slot
    int id = slot->id;
    pthread_mutex_unlock(&reg->lock);

    // init stop flag, destroy VM when stop_requested = 1
    atomic_init(&slot->vm.stop_requested, 0);

    // set up KVM VM
    if (vm_setup(&slot->vm, id, cfg) != 0)
    {
        snprintf(err_buf, err_buf_len, "vm setup failed: kernel=%s initramfs=%s",
                 cfg->kernel_path, cfg->initramfs_path);
        pthread_mutex_lock(&reg->lock);
        slot->in_use = 0;
        pthread_mutex_unlock(&reg->lock);
        return -1;
    }

    char log_path[320];
    snprintf(log_path, sizeof(log_path), "%s/vm-%d.log", reg->log_dir, id);
    // create per-VM com1 device, giving each VM its own log file
    slot->vm.com1 = com1_create(slot->vm.fd, log_path);
    if (!slot->vm.com1)
    {
        snprintf(err_buf, err_buf_len, "failed to allocate com1 device");
        vm_cleanup(&slot->vm);
        pthread_mutex_lock(&reg->lock);
        slot->in_use = 0;
        pthread_mutex_unlock(&reg->lock);
        return -1;
    }

    // init forwarding table
    memset(slot->forwards, 0, sizeof(slot->forwards));
    char ifname[16];
    slot->tap_fd = tap_create(id, ifname, sizeof(ifname));
    if (slot->tap_fd >= 0 && tap_host_configure(ifname, id) != 0)
    {
        close(slot->tap_fd);
        slot->tap_fd = -1;
    }
    // isolate VM networking to prevent one VM from routing into another VM's tap subnet
    if (slot->tap_fd >= 0 && tap_isolate_vm(ifname) != 0)
        fprintf(stderr, "registry_create_vm: failed to isolate %s from other VMs' tap devices\n", ifname);
    if (slot->tap_fd >= 0)
        snprintf(slot->net_ifname, sizeof(slot->net_ifname), "%s", ifname);
    else
        slot->net_ifname[0] = '\0';

    // create virtual network card for this vm
    // connect guest virtio-net <-> guest memory <-> host tap <-> host network
    slot->vm.net = virtio_net_create(slot->vm.fd, slot->tap_fd, id,
                                     slot->vm.mem, slot->vm.mem_size,
                                     &slot->vm.stop_requested);
    if (!slot->vm.net)
    {
        snprintf(err_buf, err_buf_len, "failed to allocate virtio-net device");
        if (slot->tap_fd >= 0)
        {
            tap_remove_isolation(slot->net_ifname);
            close(slot->tap_fd);
        }
        com1_destroy(slot->vm.com1);
        vm_cleanup(&slot->vm);
        pthread_mutex_lock(&reg->lock);
        slot->in_use = 0;
        pthread_mutex_unlock(&reg->lock);
        return -1;
    }

    pthread_mutex_lock(&slot->state_mutex);
    slot->state = VM_STATE_RUNNING;
    pthread_mutex_unlock(&slot->state_mutex);

    // start the supervisor thread for this VM
    if (pthread_create(&slot->supervisor_thread, NULL, supervisor_main, slot) != 0)
    {
        snprintf(err_buf, err_buf_len, "failed to start supervisor thread");
        atomic_store(&slot->vm.stop_requested, 1);
        virtio_net_destroy(slot->vm.net);
        if (slot->tap_fd >= 0)
        {
            tap_remove_isolation(slot->net_ifname);
            close(slot->tap_fd);
        }
        com1_destroy(slot->vm.com1);
        vm_cleanup(&slot->vm);
        pthread_mutex_lock(&reg->lock);
        slot->in_use = 0;
        pthread_mutex_unlock(&reg->lock);
        return -1;
    }

    // Publish VM so that other connections can access LIST/STATUS/DESTROY
    pthread_mutex_lock(&reg->lock);
    slot->ready = 1;
    pthread_mutex_unlock(&reg->lock);

    return id;
}

int registry_status(struct registry *reg, int id, enum vm_state *out_state,
                    struct vm_config *out_cfg, char *net_ifname_out,
                    size_t net_ifname_len)
{
    pthread_mutex_lock(&reg->lock);
    int idx = find_ready_slot_by_id_locked(reg, id);
    if (idx < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        return -1;
    }
    struct vm_slot *slot = &reg->slots[idx];
    if (out_cfg)
        *out_cfg = slot->config;
    if (net_ifname_out)
        snprintf(net_ifname_out, net_ifname_len, "%s", slot->net_ifname);
    pthread_mutex_unlock(&reg->lock);

    pthread_mutex_lock(&slot->state_mutex);
    if (out_state)
        *out_state = slot->state;
    pthread_mutex_unlock(&slot->state_mutex);
    return 0;
}

int registry_list(struct registry *reg, struct vm_list_entry *out, int max)
{
    pthread_mutex_lock(&reg->lock);
    int count = 0;
    for (int i = 0; i < REGISTRY_MAX_VMS && count < max; i++)
    {
        struct vm_slot *slot = &reg->slots[i];
        if (!slot->in_use || !slot->ready)
            continue;
        pthread_mutex_lock(&slot->state_mutex);
        out[count].id = slot->id;
        out[count].state = slot->state;
        pthread_mutex_unlock(&slot->state_mutex);
        count++;
    }
    pthread_mutex_unlock(&reg->lock);
    return count;
}

int registry_destroy_vm(struct registry *reg, int id)
{
    pthread_mutex_lock(&reg->lock);
    int idx = find_ready_slot_by_id_locked(reg, id);
    if (idx < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        return -1;
    }
    struct vm_slot *slot = &reg->slots[idx];
    pthread_mutex_unlock(&reg->lock);

    atomic_store(&slot->vm.stop_requested, 1);
    if (slot->vcpu_tid_valid)
        pthread_kill(slot->vcpu_tid, SIGUSR1);
    pthread_join(slot->supervisor_thread, NULL);

    // virtio_net_destroy joins the RX thread, which touches vm->mem - this
    // must happen before vm_cleanup() unmaps that memory below.
    virtio_net_destroy(slot->vm.net);
    for (int i = 0; i < REGISTRY_MAX_FORWARDS; i++)
    {
        if (slot->forwards[i].active)
            tap_remove_forward(slot->id, slot->forwards[i].host_port);
    }
    if (slot->tap_fd >= 0)
    {
        tap_remove_isolation(slot->net_ifname);
        close(slot->tap_fd);
    }

    vm_cleanup(&slot->vm);
    com1_destroy(slot->vm.com1);

    pthread_mutex_lock(&reg->lock);
    slot->ready = 0;
    slot->in_use = 0;
    pthread_mutex_unlock(&reg->lock);
    return 0;
}

int registry_add_forward(struct registry *reg, int id, int host_port, int guest_port,
                         char *err_buf, size_t err_buf_len)
{
    pthread_mutex_lock(&reg->lock);
    int idx = find_ready_slot_by_id_locked(reg, id);
    if (idx < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        snprintf(err_buf, err_buf_len, "no such vm: %d", id);
        return -1;
    }
    struct vm_slot *slot = &reg->slots[idx];
    if (slot->tap_fd < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        snprintf(err_buf, err_buf_len, "vm %d has no network device", id);
        return -1;
    }
    int slot_i = -1;
    for (int i = 0; i < REGISTRY_MAX_FORWARDS; i++)
    {
        if (!slot->forwards[i].active)
        {
            slot_i = i;
            break;
        }
    }
    if (slot_i < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        snprintf(err_buf, err_buf_len, "forward table full for vm %d (max %d)",
                 id, REGISTRY_MAX_FORWARDS);
        return -1;
    }
    pthread_mutex_unlock(&reg->lock);

    if (tap_add_forward(id, host_port, guest_port) != 0)
    {
        snprintf(err_buf, err_buf_len, "iptables rule setup failed");
        return -1;
    }

    pthread_mutex_lock(&reg->lock);
    slot->forwards[slot_i].active = 1;
    slot->forwards[slot_i].host_port = host_port;
    slot->forwards[slot_i].guest_port = guest_port;
    pthread_mutex_unlock(&reg->lock);
    return 0;
}

int registry_remove_forward(struct registry *reg, int id, int host_port,
                            char *err_buf, size_t err_buf_len)
{
    pthread_mutex_lock(&reg->lock);
    int idx = find_ready_slot_by_id_locked(reg, id);
    if (idx < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        snprintf(err_buf, err_buf_len, "no such vm: %d", id);
        return -1;
    }
    struct vm_slot *slot = &reg->slots[idx];
    int slot_i = -1;
    for (int i = 0; i < REGISTRY_MAX_FORWARDS; i++)
    {
        if (slot->forwards[i].active && slot->forwards[i].host_port == host_port)
        {
            slot_i = i;
            break;
        }
    }
    if (slot_i < 0)
    {
        pthread_mutex_unlock(&reg->lock);
        snprintf(err_buf, err_buf_len, "no forward for host_port %d on vm %d", host_port, id);
        return -1;
    }
    pthread_mutex_unlock(&reg->lock);

    int rc = tap_remove_forward(id, host_port);

    pthread_mutex_lock(&reg->lock);
    slot->forwards[slot_i].active = 0;
    pthread_mutex_unlock(&reg->lock);

    if (rc != 0)
    {
        snprintf(err_buf, err_buf_len, "iptables rule removal failed");
        return -1;
    }
    return 0;
}
