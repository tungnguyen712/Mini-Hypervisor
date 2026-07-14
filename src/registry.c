#include "registry.h"
#include "vcpu.h"
#include "smp.h"
#include "io.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

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
            break; // couldn't start this one; only join what actually started
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
    slot->vm.id = id;
    if (vm_setup(&slot->vm, cfg) != 0)
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
    // create per-VM com1 device
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

    pthread_mutex_lock(&slot->state_mutex);
    slot->state = VM_STATE_RUNNING;
    pthread_mutex_unlock(&slot->state_mutex);

    // start the supervisor thread for this VM
    if (pthread_create(&slot->supervisor_thread, NULL, supervisor_main, slot) != 0)
    {
        snprintf(err_buf, err_buf_len, "failed to start supervisor thread");
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
                    struct vm_config *out_cfg)
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
    pthread_join(slot->supervisor_thread, NULL);

    vm_cleanup(&slot->vm);
    com1_destroy(slot->vm.com1);

    pthread_mutex_lock(&reg->lock);
    slot->ready = 0;
    slot->in_use = 0;
    pthread_mutex_unlock(&reg->lock);
    return 0;
}
