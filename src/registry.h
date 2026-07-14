#ifndef REGISTRY_H
#define REGISTRY_H

#include "vm.h"
#include <pthread.h>

#define REGISTRY_MAX_VMS 32

enum vm_state
{
    VM_STATE_RUNNING,
    VM_STATE_STOPPED,
};

struct vm_list_entry
{
    int id;
    enum vm_state state;
};

struct vm_slot
{
    int in_use;
    int ready;
    int id;
    struct vm vm;
    struct vm_config config;
    enum vm_state state;
    int last_exit_ok;
    char last_error[128];
    pthread_mutex_t state_mutex;
    pthread_t supervisor_thread;
};

struct registry
{
    pthread_mutex_t lock;
    struct vm_slot slots[REGISTRY_MAX_VMS];
    int next_id;
    char log_dir[256];
};

void registry_init(struct registry *reg, const char *log_dir);

int registry_create_vm(struct registry *reg, const struct vm_config *cfg,
                       char *err_buf, size_t err_buf_len);

// Look up a VM by id -> check VM status and retrieve its configuration
int registry_status(struct registry *reg, int id, enum vm_state *out_state,
                    struct vm_config *out_cfg);

// return all public VMs up to max entries
int registry_list(struct registry *reg, struct vm_list_entry *out, int max);
int registry_destroy_vm(struct registry *reg, int id);

#endif // REGISTRY_H