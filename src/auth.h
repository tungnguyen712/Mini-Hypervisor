#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

#define AUTH_MAX_TOKENS 64
#define AUTH_TOKEN_MAX_LEN 128
#define AUTH_OWNER_MAX_LEN 64

// Daemon startup
//     ↓
// auth_load()
//     ↓
// Read allowed tokens from a file
//     ↓
// Store them in auth_table
//     ↓
// Client connects and sends a token
//     ↓
// auth_check()
//     ↓
// Valid token → identify owner
// Invalid token → reject client

struct auth_entry
{
    char token[AUTH_TOKEN_MAX_LEN];
    char owner[AUTH_OWNER_MAX_LEN];
};

// fixed-size array holding all allowed credentials
struct auth_table
{
    struct auth_entry entries[AUTH_MAX_TOKENS];
    int count;
};

// Reads "<token> <owner>" pairs from path, one per line
int auth_load(struct auth_table *tbl, const char *path, char *err_buf, size_t err_buf_len);

// checks whether a client-provided token appears in the loaded table
int auth_check(const struct auth_table *tbl, const char *token, char *owner_out, size_t owner_len);

#endif // AUTH_H
