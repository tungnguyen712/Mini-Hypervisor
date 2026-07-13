#include "vm.h"
#include "vcpu.h"
#include "smp.h"
#include "io.h"
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

// 512 MB of guest RAM
#define GUEST_MEM_SIZE 0x20000000

int main()
{
    struct vm vm;
    struct vcpu vcpu0;
    uint64_t initrd_addr;
    uint32_t initrd_size;

    vm_init(&vm, GUEST_MEM_SIZE);

    load_kernel_bzimage(&vm, "bzImage");
    load_initramfs(&vm, "initramfs.cpio.gz", &initrd_addr, &initrd_size);
    setup_boot_params(&vm, "bzImage",
                      "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200"
                      " rdinit=/init nokaslr",
                      initrd_addr, initrd_size);

    com1_set_vm_fd(vm.fd);
    com1_start_input_thread();

    struct vcpu_thread_arg arg0 = {
        .vm = &vm,
        .vcpu = &vcpu0,
        .vcpu_id = 0,
        .rip = 0,
        .use_linux_entry = 1,
    };

    pthread_t thread0;
    if (pthread_create(&thread0, NULL, vcpu_thread_main, &arg0) != 0)
    {
        perror("pthread_create vcpu0");
        vm_cleanup(&vm);
        return 1;
    }
    pthread_join(thread0, NULL);

    vm_cleanup(&vm);
    return 0;
}