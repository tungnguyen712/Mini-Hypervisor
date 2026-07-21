

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
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

static int try_send_line(int fd, const char *line)
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
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
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

static void auth(int fd, const char *token, char *line, size_t line_size)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AUTH %s\n", token);
    send_line(fd, cmd);
    read_line(fd, line, line_size);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    const char *sock_path = argc > 1 ? argv[1] : "mini_hv.sock";
    const char *token_a = argc > 2 ? argv[2] : NULL;
    const char *token_b = argc > 3 ? argv[3] : NULL;
    const char *kernel_path = argc > 4 ? argv[4] : "bzImage";
    const char *initramfs_path = argc > 5 ? argv[5] : "initramfs.cpio.gz";

    if (!token_a || !token_b)
    {
        fprintf(stderr, "usage: %s <sock_path> <token_a> <token_b> [kernel] [initramfs]\n"
                        "  token_a/token_b must be two distinct tenants' AUTH tokens\n",
                argv[0]);
        return 1;
    }

    char line[1024];
    char cmd[1024];

    // 0. Unauthenticated connections can't do anything but AUTH.
    int fd = connect_unix(sock_path);
    send_line(fd, "LIST\n");
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "ERR unauthenticated", 20) == 0, "LIST before AUTH returns ERR unauthenticated");

    auth(fd, "not-a-real-token", line, sizeof(line));
    check(strncmp(line, "ERR", 3) == 0, "AUTH with a bad token returns ERR");

    auth(fd, token_a, line, sizeof(line));
    check(strncmp(line, "OK authenticated", 16) == 0, "AUTH with a valid token returns OK");

    auth(fd, token_a, line, sizeof(line));
    check(strncmp(line, "ERR already authenticated", 26) == 0, "re-AUTH on the same connection returns ERR");

    // 1. CREATE a VM as tenant A.
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

    // 4. A different tenant (B) must not see or touch tenant A's VM: same
    // "no such vm" error whether the id is wrong or just not theirs, so B
    // can't use these calls to fingerprint ids that belong to someone else.
    int fd_b = connect_unix(sock_path);
    auth(fd_b, token_b, line, sizeof(line));
    check(strncmp(line, "OK authenticated", 16) == 0, "tenant B authenticates with their own token");

    send_line(fd_b, "LIST\n");
    read_line(fd_b, line, sizeof(line));
    check(strcmp(line, "OK count=0") == 0, "tenant B's LIST does not show tenant A's VM");

    snprintf(cmd, sizeof(cmd), "STATUS %d\n", id);
    send_line(fd_b, cmd);
    read_line(fd_b, line, sizeof(line));
    check(strncmp(line, "ERR no such vm:", 15) == 0, "tenant B's STATUS on A's vm returns 'no such vm'");

    snprintf(cmd, sizeof(cmd), "DESTROY %d\n", id);
    send_line(fd_b, cmd);
    read_line(fd_b, line, sizeof(line));
    check(strncmp(line, "ERR no such vm:", 15) == 0, "tenant B's DESTROY on A's vm returns 'no such vm' (not destroyed)");
    close(fd_b);

    // 5. Tenant A (the actual owner) can still see and destroy it.
    snprintf(cmd, sizeof(cmd), "STATUS %d\n", id);
    send_line(fd, cmd);
    read_line(fd, line, sizeof(line));
    check(strstr(line, "state=running") != NULL, "owner's STATUS on their own vm still works after B's attempt");

    snprintf(cmd, sizeof(cmd), "DESTROY %d\n", id);
    send_line(fd, cmd);
    read_line(fd, line, sizeof(line));
    snprintf(cmd, sizeof(cmd), "OK id=%d destroyed", id);
    check(strcmp(line, cmd) == 0, "DESTROY acknowledges the VM");

    // 6. STATUS on the destroyed id should now fail.
    snprintf(cmd, sizeof(cmd), "STATUS %d\n", id);
    send_line(fd, cmd);
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "ERR", 3) == 0, "STATUS on a destroyed id returns ERR");

    // 7. A deliberately-bad CREATE must not take the daemon down -- this is
    // the regression test for removing exit(1) from vm.c/vcpu.c.
    send_line(fd, "CREATE kernel=/no/such/path initramfs=/no/such/path\n");
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "ERR", 3) == 0, "CREATE with a bad kernel path returns ERR");

    // 7b. A CREATE that tries to escape the images directory must be
    // rejected too, and for the same reason the daemon must survive it.
    send_line(fd, "CREATE kernel=../../../../etc/passwd initramfs=initramfs.cpio.gz\n");
    read_line(fd, line, sizeof(line));
    check(strncmp(line, "ERR", 3) == 0, "CREATE with a path-traversal kernel= returns ERR");

    send_line(fd, "LIST\n");
    int n = read_line(fd, line, sizeof(line));
    check(n > 0 && strncmp(line, "OK count=", 9) == 0,
          "daemon still answers LIST after failed CREATEs (didn't crash)");
    count = atoi(line + 9);
    for (int i = 0; i < count; i++)
        read_line(fd, line, sizeof(line));

    close(fd);

    // 8. A connection that keeps guessing AUTH tokens gets cut off rather
    // than allowed to guess forever.
    int fd_brute = connect_unix(sock_path);
    int cap_hit = 0;
    for (int i = 0; i < 6; i++)
    {
        if (try_send_line(fd_brute, "AUTH definitely-not-a-real-token\n") != 0)
        {
            cap_hit = 1;
            break;
        }
        int rn = read_line(fd_brute, line, sizeof(line));
        if (rn <= 0)
        {
            cap_hit = 1;
            break;
        }
        if (strncmp(line, "ERR", 3) != 0)
            break; // unexpected reply; not what this check is looking for
    }
    check(cap_hit, "connection is closed after repeated bad AUTH attempts");
    close(fd_brute);

    if (failures > 0)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
