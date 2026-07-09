#ifndef VM_H
#define VM_H

#include <stddef.h>

struct vm
{
    int fd;
    int sys_fd;
    void *mem;
    size_t mem_size;
};

void vm_init(struct vm *vm, size_t mem_size);
void vm_cleanup(struct vm *vm);
void load_payload(struct vm *vm, const void *payload, size_t size);

#endif // VM_H