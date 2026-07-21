// Standalone helper: mints a new random token for a tenant and appends
// "<token> <owner>" to the given token file.

// Usage: mhv-token <tokens-file> <owner>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define TOKEN_BYTES 24

static int owner_is_valid(const char *owner)
{
    if (*owner == '\0')
        return 0;
    for (const char *p = owner; *p; p++)
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '#')
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <tokens-file> <owner>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    const char *owner = argv[2];

    if (!owner_is_valid(owner))
    {
        fprintf(stderr, "%s: owner must be non-empty with no whitespace/'#'\n", argv[0]);
        return 1;
    }

    unsigned char raw[TOKEN_BYTES];
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd < 0)
    {
        perror("open /dev/urandom");
        return 1;
    }
    ssize_t off = 0;
    while (off < TOKEN_BYTES)
    {
        ssize_t n = read(rfd, raw + off, (size_t)(TOKEN_BYTES - off));
        if (n <= 0)
        {
            perror("read /dev/urandom");
            close(rfd);
            return 1;
        }
        off += n;
    }
    close(rfd);
    // generate random secret token as hex string
    char token[TOKEN_BYTES * 2 + 1];
    for (int i = 0; i < TOKEN_BYTES; i++)
        snprintf(token + i * 2, 3, "%02x", raw[i]);

    // add to token files carry live credentials
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0)
    {
        perror(path);
        return 1;
    }
    fchmod(fd, 0600);

    char line[256];
    int len = snprintf(line, sizeof(line), "%s %s\n", token, owner);
    if (write(fd, line, (size_t)len) != len)
    {
        perror("write");
        close(fd);
        return 1;
    }
    close(fd);

    fprintf(stderr, "added token for owner '%s' to %s\n", owner, path);
    printf("%s\n", token);
    return 0;
}
