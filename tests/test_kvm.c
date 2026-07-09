#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

int main(void)
{
    int kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0)
    {
        perror("open /dev/kvm");
        return 1;
    }

    int api_ver = ioctl(kvm_fd, KVM_GET_API_VERSION, NULL);
    if (api_ver < 0)
    {
        perror("KVM_GET_API_VERSION");
        return 1;
    }
    printf("KVM API version: %d (expected 12)\n", api_ver);
    if (api_ver != 12)
    {
        fprintf(stderr, "Warning: unexpected API version, proceed with caution\n");
    }

    int has_user_memory = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    printf("KVM_CAP_USER_MEMORY supported: %d\n", has_user_memory);

    int nr_vcpus = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_NR_VCPUS);
    printf("Recommended max VCPUs: %d\n", nr_vcpus);

    close(kvm_fd);
    printf("Phase 0 check passed.\n");
    return 0;
}