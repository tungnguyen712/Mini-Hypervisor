

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond)
        printf("PASS: %s\n", what);
    else
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static int connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        exit(1);
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        fprintf(stderr, "is mini_hv running on socket '%s'?\n", path);
        exit(1);
    }
    return fd;
}

static void send_line(int fd, const char *line)
{
    size_t len = strlen(line);
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, line + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            perror("write");
            exit(1);
        }
        off += (size_t)n;
    }
}

static int read_line(int fd, char *buf, size_t bufsize)
{
    size_t len = 0;
    for (;;)
    {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0)
            return -1;
        if (c == '\n')
            break;
        if (len + 1 >= bufsize)
            return -1;
        buf[len++] = c;
    }
    buf[len] = '\0';
    return (int)len;
}

int main(int argc, char **argv)
{
    const char *sock_path = argc > 1 ? argv[1] : "mini_hv.sock";
    const char *kernel_path = argc > 2 ? argv[2] : "bzImage";
    const char *initramfs_path = argc > 3 ? argv[3] : "initramfs.cpio.gz";

    char line[1024];
    char cmd[1024];

    // 1. CREATE a VM.
    int fd = connect_unix(sock_path);
    snprintf(cmd, sizeof(cmd), "CREATE kernel=%s initramfs=%s\n", kernel_path, initramfs_path);
    send_line(fd, cmd);
    check(read_line(fd, line, sizeof(line)) > 0 && strncmp(line, "OK id=", 6) == 0,
          "CREATE returns OK id=<n>");
    int id = atoi(line + 6);
    check(id > 0, "CREATE returned a positive id");

    // 2. LIST should show it running.
    send_line(fd, "LIST\n");
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "OK count=", 9) == 0, "LIST returns OK count=<n>");
    int count = atoi(line + 9);
    int found_running = 0;
    for (int i = 0; i < count; i++)
    {
        read_line(fd, line, sizeof(line));
        char want[64];
        snprintf(want, sizeof(want), "id=%d state=running", id);
        if (strcmp(line, want) == 0)
            found_running = 1;
    }
    check(found_running, "LIST shows the new VM as running");

    // 3. STATUS should report the same VM as running with the paths we gave it.
    snprintf(cmd, sizeof(cmd), "STATUS %d\n", id);
    send_line(fd, cmd);
    read_line(fd, line, sizeof(line));
    check(strstr(line, "state=running") != NULL, "STATUS reports state=running");

    // 4. DESTROY it.
    snprintf(cmd, sizeof(cmd), "DESTROY %d\n", id);
    send_line(fd, cmd);
    read_line(fd, line, sizeof(line));
    snprintf(cmd, sizeof(cmd), "OK id=%d destroyed", id);
    check(strcmp(line, cmd) == 0, "DESTROY acknowledges the VM");

    // 5. STATUS on the destroyed id should now fail.
    snprintf(cmd, sizeof(cmd), "STATUS %d\n", id);
    send_line(fd, cmd);
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "ERR", 3) == 0, "STATUS on a destroyed id returns ERR");

    // 6. A deliberately-bad CREATE must not take the daemon down -- this is
    // the regression test for removing exit(1) from vm.c/vcpu.c.
    send_line(fd, "CREATE kernel=/no/such/path initramfs=/no/such/path\n");
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "ERR", 3) == 0, "CREATE with a bad kernel path returns ERR");

    send_line(fd, "LIST\n");
    int n = read_line(fd, line, sizeof(line));
    check(n > 0 && strncmp(line, "OK count=", 9) == 0,
          "daemon still answers LIST after a failed CREATE (didn't crash)");
    count = atoi(line + 9);
    for (int i = 0; i < count; i++)
        read_line(fd, line, sizeof(line));

    close(fd);

    if (failures > 0)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
