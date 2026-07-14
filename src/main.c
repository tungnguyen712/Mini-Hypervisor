#include "registry.h"
#include "server.h"

#include <stdio.h>
#include <sys/stat.h>

#define DEFAULT_SOCK_PATH "mini_hv.sock"
#define DEFAULT_LOG_DIR "vm-logs"

int main(int argc, char **argv)
{
    const char *sock_path = (argc > 1) ? argv[1] : DEFAULT_SOCK_PATH;

    mkdir(DEFAULT_LOG_DIR, 0755);

    struct registry reg;
    registry_init(&reg, DEFAULT_LOG_DIR);

    fprintf(stderr, "mini_hv: listening on %s (per-VM logs in %s/)\n",
            sock_path, DEFAULT_LOG_DIR);

    if (server_run(sock_path, &reg) != 0)
    {
        fprintf(stderr, "mini_hv: failed to start server on %s\n", sock_path);
        return 1;
    }
    return 0;
}