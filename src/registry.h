#ifndef REGISTRY_H
#define REGISTRY_H

#include "vm.h"
#include <pthread.h>

#define REGISTRY_MAX_VMS 32
#define REGISTRY_MAX_FORWARDS 8 // max port forwards per VM

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

struct port_forward
{
    int active;
    int host_port;
    int guest_port;
};

struct vm_slot
{
    // slot identity
    int in_use;
    int ready;
    int id;
    char owner[64]; // set once at CREATE, never mutated after
    // VM instance and configuration
    struct vm vm;
    struct vm_config config;
    // executation state
    enum vm_state state;
    int last_exit_ok;
    char last_error[128];
    pthread_mutex_t state_mutex;
    // registry threads -> supervisor threads -> vcpu threads
    pthread_t supervisor_thread;
    pthread_t vcpu_tid;
    int vcpu_tid_valid;
    // networking
    int tap_fd;
    char net_ifname[16]; // interface name (tap<id>) for this VM
    struct port_forward forwards[REGISTRY_MAX_FORWARDS];
};

// fixed size table to manage all VMs, their state, and their network forwards
struct registry
{
    pthread_mutex_t lock;
    struct vm_slot slots[REGISTRY_MAX_VMS];
    int next_id;
    char log_dir[256];
};

void registry_init(struct registry *reg, const char *log_dir);

// owner is the authenticated caller's identity (from AUTH); the created VM
// is recorded as belonging to it and every lookup below is scoped to it.
int registry_create_vm(struct registry *reg, const struct vm_config *cfg,
                       const char *owner, char *err_buf, size_t err_buf_len);

// Look up a VM by id -> check VM status and retrieve its configuration.
// Fails (as if the VM didn't exist) if it belongs to a different owner.
int registry_status(struct registry *reg, int id, const char *owner,
                    enum vm_state *out_state, struct vm_config *out_cfg,
                    char *net_ifname_out, size_t net_ifname_len);

// return the calling owner's VMs, up to max entries
int registry_list(struct registry *reg, const char *owner, struct vm_list_entry *out, int max);
int registry_destroy_vm(struct registry *reg, int id, const char *owner);

// Add/remove a port forwarding, also rejects host_port
// values already forwarded to any *other* VM (regardless of owner) to
// prevent one tenant's FORWARD from hijacking traffic another
// tenant's forward is already claiming.
int registry_add_forward(struct registry *reg, int id, const char *owner,
                         int host_port, int guest_port,
                         char *err_buf, size_t err_buf_len);
int registry_remove_forward(struct registry *reg, int id, const char *owner,
                            int host_port, char *err_buf, size_t err_buf_len);

#endif // REGISTRY_H