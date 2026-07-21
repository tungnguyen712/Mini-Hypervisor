#include "registry.h"
#include "server.h"
#include "tap.h"
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>

static void noop_handler(int sig) { (void)sig; }

#define DEFAULT_SOCK_PATH "mini_hv.sock"
#define DEFAULT_LOG_DIR "vm-logs" // per-vm logs directory
#define DEFAULT_IMAGES_DIR "."

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s --tokens <path> [--images-dir <path>] [sock_path]\n"
                    "--tokens is required: a file of '<token> <owner>' lines, one per tenant.\n"
                    "Mint one with: ./mhv-token <tokens-file> <owner>\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *sock_path = DEFAULT_SOCK_PATH;
    const char *tokens_path = NULL;
    const char *images_dir = DEFAULT_IMAGES_DIR;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--tokens") == 0 && i + 1 < argc)
            tokens_path = argv[++i];
        else if (strcmp(argv[i], "--images-dir") == 0 && i + 1 < argc)
            images_dir = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else
            sock_path = argv[i];
    }

    if (!tokens_path)
    {
        fprintf(stderr, "mini_hv: --tokens is required; refusing to start with no access control\n");
        usage(argv[0]);
        return 1;
    }

    struct auth_table auth;
    char auth_err[256];
    if (auth_load(&auth, tokens_path, auth_err, sizeof(auth_err)) != 0)
    {
        fprintf(stderr, "mini_hv: %s\n", auth_err);
        return 1;
    }

    struct sigaction sa = {.sa_handler = noop_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    mkdir(DEFAULT_LOG_DIR, 0755);
    mkdir(images_dir, 0755);

    // config networking setup for whole daemon
    if (tap_ensure_nat() != 0)
        fprintf(stderr, "mini_hv: NAT setup failed; guests will have no network reachability beyond their own tap link\n");

    // init registry
    struct registry reg;
    registry_init(&reg, DEFAULT_LOG_DIR);

    fprintf(stderr, "mini_hv: listening on %s (per-VM logs in %s/, images confined to %s/, %d token(s) loaded)\n",
            sock_path, DEFAULT_LOG_DIR, images_dir, auth.count);

    // start server loop
    if (server_run(sock_path, &reg, &auth, images_dir) != 0)
    {
        fprintf(stderr, "mini_hv: failed to start server on %s\n", sock_path);
        return 1;
    }
    return 0;
}