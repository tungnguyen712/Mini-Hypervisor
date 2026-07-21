#include "server.h"
#include "registry.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#define LINE_MAX_LEN 1024
#define RESP_MAX_LEN 4096

// every client connection gets its own thread
struct conn_arg
{
    int fd;
    struct registry *reg;
};

// convert state to state text sent to client
static const char *state_name(enum vm_state s)
{
    return s == VM_STATE_RUNNING ? "running" : "stopped";
}

// send complete response to client
static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, buf + off, len - off);
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
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) // EOF
            return len > 0 ? -1 : -1;
        if (c == '\n')
            break;
        if (len + 1 >= bufsize)
            return -1;
        buf[len++] = c;
    }
    if (len > 0 && buf[len - 1] == '\r')
        len--;
    buf[len] = '\0';
    return (int)len;
}

// parses arguments from a CREATE command into config
static void parse_create_args(char *args, struct vm_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    char *saveptr = NULL;
    for (char *tok = strtok_r(args, " \t", &saveptr); tok; tok = strtok_r(NULL, " \t", &saveptr))
    {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        if (strcmp(key, "kernel") == 0)
            snprintf(cfg->kernel_path, sizeof(cfg->kernel_path), "%s", val);
        else if (strcmp(key, "initramfs") == 0)
            snprintf(cfg->initramfs_path, sizeof(cfg->initramfs_path), "%s", val);
        else if (strcmp(key, "disk") == 0)
            snprintf(cfg->disk_path, sizeof(cfg->disk_path), "%s", val);
    }
}

static void handle_create(struct registry *reg, char *args, char *resp, size_t resp_size)
{
    struct vm_config cfg;
    parse_create_args(args, &cfg);

    // validate required fields from config parsed above
    if (cfg.kernel_path[0] == '\0')
    {
        snprintf(resp, resp_size, "ERR missing required field: kernel\n");
        return;
    }
    if (cfg.initramfs_path[0] == '\0')
    {
        snprintf(resp, resp_size, "ERR missing required field: initramfs\n");
        return;
    }

    char err_buf[128];
    int id = registry_create_vm(reg, &cfg, err_buf, sizeof(err_buf));
    if (id < 0)
        snprintf(resp, resp_size, "ERR %s\n", err_buf);
    else
        snprintf(resp, resp_size, "OK id=%d\n", id);
}

static void handle_list(struct registry *reg, char *resp, size_t resp_size)
{
    struct vm_list_entry entries[REGISTRY_MAX_VMS];
    int count = registry_list(reg, entries, REGISTRY_MAX_VMS);

    int off = snprintf(resp, resp_size, "OK count=%d\n", count);
    for (int i = 0; i < count && off < (int)resp_size; i++)
    {
        off += snprintf(resp + off, resp_size - (size_t)off, "id=%d state=%s\n",
                        entries[i].id, state_name(entries[i].state));
    }
}

static void handle_status(struct registry *reg, const char *args, char *resp, size_t resp_size)
{
    int id = atoi(args);
    enum vm_state state;
    struct vm_config cfg;
    char net_ifname[16];
    if (registry_status(reg, id, &state, &cfg, net_ifname, sizeof(net_ifname)) != 0)
    {
        snprintf(resp, resp_size, "ERR no such vm: %d\n", id);
        return;
    }
    snprintf(resp, resp_size, "OK id=%d state=%s kernel=%s initramfs=%s disk=%s net=%s\n",
             id, state_name(state), cfg.kernel_path, cfg.initramfs_path,
             cfg.disk_path[0] ? cfg.disk_path : "-",
             net_ifname[0] ? net_ifname : "-");
}

static void handle_forward(struct registry *reg, char *args, char *resp, size_t resp_size)
{
    // split args into id and host_port/guest_port
    char *saveptr = NULL;
    char *id_s = strtok_r(args, " \t", &saveptr);
    char *host_port_s = strtok_r(NULL, " \t", &saveptr);
    char *guest_port_s = strtok_r(NULL, " \t", &saveptr);
    if (!id_s || !host_port_s || !guest_port_s)
    {
        snprintf(resp, resp_size, "ERR usage: FORWARD <id> <host_port> <guest_port>\n");
        return;
    }
    int id = atoi(id_s);
    int host_port = atoi(host_port_s);
    int guest_port = atoi(guest_port_s);

    char err_buf[128];
    if (registry_add_forward(reg, id, host_port, guest_port, err_buf, sizeof(err_buf)) != 0)
        snprintf(resp, resp_size, "ERR %s\n", err_buf);
    else
        snprintf(resp, resp_size, "OK forwarded host_port=%d -> vm=%d guest_port=%d\n",
                 host_port, id, guest_port);
}

static void handle_unforward(struct registry *reg, char *args, char *resp, size_t resp_size)
{
    // split args into id and host_port
    char *saveptr = NULL;
    char *id_s = strtok_r(args, " \t", &saveptr);
    char *host_port_s = strtok_r(NULL, " \t", &saveptr);
    if (!id_s || !host_port_s)
    {
        snprintf(resp, resp_size, "ERR usage: UNFORWARD <id> <host_port>\n");
        return;
    }
    int id = atoi(id_s);
    int host_port = atoi(host_port_s);

    char err_buf[128];
    if (registry_remove_forward(reg, id, host_port, err_buf, sizeof(err_buf)) != 0)
        snprintf(resp, resp_size, "ERR %s\n", err_buf);
    else
        snprintf(resp, resp_size, "OK unforwarded host_port=%d\n", host_port);
}

static void handle_destroy(struct registry *reg, const char *args, char *resp, size_t resp_size)
{
    int id = atoi(args);
    if (registry_destroy_vm(reg, id) != 0)
        snprintf(resp, resp_size, "ERR no such vm: %d\n", id);
    else
        snprintf(resp, resp_size, "OK id=%d destroyed\n", id);
}

static void handle_command(struct registry *reg, char *line, char *resp, size_t resp_size)
{
    char *saveptr = NULL;
    char *cmd = strtok_r(line, " \t", &saveptr);
    char *rest = strtok_r(NULL, "", &saveptr);
    if (!cmd)
    {
        snprintf(resp, resp_size, "ERR empty command\n");
        return;
    }
    if (strcmp(cmd, "CREATE") == 0)
        handle_create(reg, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "LIST") == 0)
        handle_list(reg, resp, resp_size);
    else if (strcmp(cmd, "STATUS") == 0)
        handle_status(reg, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "DESTROY") == 0)
        handle_destroy(reg, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "FORWARD") == 0)
        handle_forward(reg, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "UNFORWARD") == 0)
        handle_unforward(reg, rest ? rest : "", resp, resp_size);
    else
        snprintf(resp, resp_size, "ERR unknown command: %s\n", cmd);
}

// handle one connected client continuously
static void *conn_thread_main(void *arg)
{
    struct conn_arg *carg = (struct conn_arg *)arg;
    int fd = carg->fd;
    struct registry *reg = carg->reg;
    free(carg);

    char line[LINE_MAX_LEN];
    char resp[RESP_MAX_LEN];
    for (;;)
    {
        int len = read_line(fd, line, sizeof(line));
        if (len < 0)
            break; // EOF, error, or line too long: close the connection
        handle_command(reg, line, resp, sizeof(resp));
        if (write_all(fd, resp, strlen(resp)) != 0)
            break;
    }

    close(fd);
    return NULL;
}

int server_run(const char *sock_path, struct registry *reg)
{
    unlink(sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(lfd);
        return -1;
    }

    if (listen(lfd, 16) < 0)
    {
        perror("listen");
        close(lfd);
        return -1;
    }

    for (;;)
    {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0)
        {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        struct conn_arg *carg = malloc(sizeof(*carg));
        if (!carg)
        {
            close(cfd);
            continue;
        }
        carg->fd = cfd;
        carg->reg = reg;

        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread_main, carg) != 0)
        {
            perror("pthread_create conn");
            close(cfd);
            free(carg);
            continue;
        }
        pthread_detach(tid);
    }
}