#define _DEFAULT_SOURCE // for realpath() under -std=c11

#include "server.h"
#include "registry.h"
#include "vm.h"
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#define LINE_MAX_LEN 1024
#define RESP_MAX_LEN 4096
#define AUTH_MAX_FAILURES 5 // connection is closed after this many bad AUTH attempts

// every client connection gets its own thread
struct conn_arg
{
    int fd;
    struct registry *reg;
    const struct auth_table *auth;
    const char *images_dir;
};

// per-connection identity: one owner per connection, set once by AUTH and
// never changed for the lifetime of the connection (no re-auth/impersonation
// mid-connection).
struct conn_state
{
    int authenticated;
    char owner[AUTH_OWNER_MAX_LEN];
    int auth_failures;
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

static int confine_path(const char *images_dir, const char *raw_path, char *out, size_t out_len)
{
    if (!raw_path || raw_path[0] == '\0')
        return -1;

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof(joined), "%s/%s", images_dir, raw_path) >= (int)sizeof(joined))
        return -1;

    char real_base[PATH_MAX];
    char real_target[PATH_MAX];
    if (!realpath(images_dir, real_base))
        return -1;
    if (!realpath(joined, real_target))
        return -1;

    size_t base_len = strlen(real_base);
    if (strncmp(real_target, real_base, base_len) != 0)
        return -1;
    if (real_target[base_len] != '/') // must be a file *inside* the dir, not the dir itself
        return -1;

    snprintf(out, out_len, "%s", real_target);
    return 0;
}

static void handle_create(struct registry *reg, const char *images_dir, const char *owner,
                          char *args, char *resp, size_t resp_size)
{
    struct vm_config raw;
    parse_create_args(args, &raw);

    // validate required fields from config parsed above
    if (raw.kernel_path[0] == '\0')
    {
        snprintf(resp, resp_size, "ERR missing required field: kernel\n");
        return;
    }
    if (raw.initramfs_path[0] == '\0')
    {
        snprintf(resp, resp_size, "ERR missing required field: initramfs\n");
        return;
    }

    struct vm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (confine_path(images_dir, raw.kernel_path, cfg.kernel_path, sizeof(cfg.kernel_path)) != 0)
    {
        snprintf(resp, resp_size, "ERR path outside images directory: kernel\n");
        return;
    }
    if (confine_path(images_dir, raw.initramfs_path, cfg.initramfs_path, sizeof(cfg.initramfs_path)) != 0)
    {
        snprintf(resp, resp_size, "ERR path outside images directory: initramfs\n");
        return;
    }
    if (raw.disk_path[0] != '\0' &&
        confine_path(images_dir, raw.disk_path, cfg.disk_path, sizeof(cfg.disk_path)) != 0)
    {
        snprintf(resp, resp_size, "ERR path outside images directory: disk\n");
        return;
    }

    char err_buf[128];
    int id = registry_create_vm(reg, &cfg, owner, err_buf, sizeof(err_buf));
    if (id < 0)
        snprintf(resp, resp_size, "ERR %s\n", err_buf);
    else
        snprintf(resp, resp_size, "OK id=%d\n", id);
}

static void handle_list(struct registry *reg, const char *owner, char *resp, size_t resp_size)
{
    struct vm_list_entry entries[REGISTRY_MAX_VMS];
    int count = registry_list(reg, owner, entries, REGISTRY_MAX_VMS);

    int off = snprintf(resp, resp_size, "OK count=%d\n", count);
    for (int i = 0; i < count && off < (int)resp_size; i++)
    {
        off += snprintf(resp + off, resp_size - (size_t)off, "id=%d state=%s\n",
                        entries[i].id, state_name(entries[i].state));
    }
}

static void handle_status(struct registry *reg, const char *owner, const char *args,
                          char *resp, size_t resp_size)
{
    int id = atoi(args);
    enum vm_state state;
    struct vm_config cfg;
    char net_ifname[16];
    if (registry_status(reg, id, owner, &state, &cfg, net_ifname, sizeof(net_ifname)) != 0)
    {
        snprintf(resp, resp_size, "ERR no such vm: %d\n", id);
        return;
    }
    snprintf(resp, resp_size, "OK id=%d state=%s kernel=%s initramfs=%s disk=%s net=%s\n",
             id, state_name(state), cfg.kernel_path, cfg.initramfs_path,
             cfg.disk_path[0] ? cfg.disk_path : "-",
             net_ifname[0] ? net_ifname : "-");
}

static void handle_forward(struct registry *reg, const char *owner, char *args,
                           char *resp, size_t resp_size)
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
    if (registry_add_forward(reg, id, owner, host_port, guest_port, err_buf, sizeof(err_buf)) != 0)
        snprintf(resp, resp_size, "ERR %s\n", err_buf);
    else
        snprintf(resp, resp_size, "OK forwarded host_port=%d -> vm=%d guest_port=%d\n",
                 host_port, id, guest_port);
}

static void handle_unforward(struct registry *reg, const char *owner, char *args,
                             char *resp, size_t resp_size)
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
    if (registry_remove_forward(reg, id, owner, host_port, err_buf, sizeof(err_buf)) != 0)
        snprintf(resp, resp_size, "ERR %s\n", err_buf);
    else
        snprintf(resp, resp_size, "OK unforwarded host_port=%d\n", host_port);
}

static void handle_destroy(struct registry *reg, const char *owner, const char *args,
                           char *resp, size_t resp_size)
{
    int id = atoi(args);
    if (registry_destroy_vm(reg, id, owner) != 0)
        snprintf(resp, resp_size, "ERR no such vm: %d\n", id);
    else
        snprintf(resp, resp_size, "OK id=%d destroyed\n", id);
}

// AUTH is the only command accepted before authentication, and is rejected
// once already authenticated (one owner per connection, no re-auth).
static void handle_auth(const struct auth_table *auth, struct conn_state *cs,
                        char *args, char *resp, size_t resp_size)
{
    if (cs->authenticated)
    {
        snprintf(resp, resp_size, "ERR already authenticated\n");
        return;
    }

    char *saveptr = NULL;
    char *token = strtok_r(args, " \t", &saveptr);
    char owner[AUTH_OWNER_MAX_LEN];
    if (token && auth_check(auth, token, owner, sizeof(owner)) == 0)
    {
        cs->authenticated = 1;
        snprintf(cs->owner, sizeof(cs->owner), "%s", owner);
        snprintf(resp, resp_size, "OK authenticated as %s\n", owner);
    }
    else
    {
        cs->auth_failures++;
        snprintf(resp, resp_size, "ERR invalid token\n");
    }
}

// Returns 0 to keep the connection open, -1 to close it (used once a
// connection has exceeded AUTH_MAX_FAILURES bad tokens).
static int handle_command(struct registry *reg, const struct auth_table *auth,
                          const char *images_dir, struct conn_state *cs,
                          char *line, char *resp, size_t resp_size)
{
    char *saveptr = NULL;
    char *cmd = strtok_r(line, " \t", &saveptr);
    char *rest = strtok_r(NULL, "", &saveptr);
    if (!cmd)
    {
        snprintf(resp, resp_size, "ERR empty command\n");
        return 0;
    }

    if (strcmp(cmd, "AUTH") == 0)
    {
        handle_auth(auth, cs, rest ? rest : "", resp, resp_size);
        return cs->auth_failures >= AUTH_MAX_FAILURES ? -1 : 0;
    }

    if (!cs->authenticated)
    {
        snprintf(resp, resp_size, "ERR unauthenticated\n");
        return 0;
    }

    if (strcmp(cmd, "CREATE") == 0)
        handle_create(reg, images_dir, cs->owner, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "LIST") == 0)
        handle_list(reg, cs->owner, resp, resp_size);
    else if (strcmp(cmd, "STATUS") == 0)
        handle_status(reg, cs->owner, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "DESTROY") == 0)
        handle_destroy(reg, cs->owner, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "FORWARD") == 0)
        handle_forward(reg, cs->owner, rest ? rest : "", resp, resp_size);
    else if (strcmp(cmd, "UNFORWARD") == 0)
        handle_unforward(reg, cs->owner, rest ? rest : "", resp, resp_size);
    else
        snprintf(resp, resp_size, "ERR unknown command: %s\n", cmd);
    return 0;
}

// handle one connected client continuously
static void *conn_thread_main(void *arg)
{
    struct conn_arg *carg = (struct conn_arg *)arg;
    int fd = carg->fd;
    struct registry *reg = carg->reg;
    const struct auth_table *auth = carg->auth;
    const char *images_dir = carg->images_dir;
    free(carg);

    struct conn_state cs;
    memset(&cs, 0, sizeof(cs));

    char line[LINE_MAX_LEN];
    char resp[RESP_MAX_LEN];
    for (;;)
    {
        int len = read_line(fd, line, sizeof(line));
        if (len < 0)
            break; // EOF, error, or line too long: close the connection
        int rc = handle_command(reg, auth, images_dir, &cs, line, resp, sizeof(resp));
        if (write_all(fd, resp, strlen(resp)) != 0)
            break;
        if (rc < 0)
            break; // too many failed auth attempts
    }

    close(fd);
    return NULL;
}

int server_run(const char *sock_path, struct registry *reg,
               const struct auth_table *auth, const char *images_dir)
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
        carg->auth = auth;
        carg->images_dir = images_dir;

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