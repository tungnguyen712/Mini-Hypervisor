#include "auth.h"

#include <stdio.h>
#include <string.h>

// Fixed-time comparison of two tokens
// every byte is compared regardless of where the first mismatch is, so how
// long the comparison takes doesn't depend on how many leading bytes of a
// guess were correct.
static int fixed_time_eq(const char a[AUTH_TOKEN_MAX_LEN], const char b[AUTH_TOKEN_MAX_LEN])
{
    volatile unsigned char diff = 0;
    for (int i = 0; i < AUTH_TOKEN_MAX_LEN; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

// called once during daemon startup
int auth_load(struct auth_table *tbl, const char *path, char *err_buf, size_t err_buf_len)
{
    // clear auth table before loading
    memset(tbl, 0, sizeof(*tbl));

    // open token file
    FILE *f = fopen(path, "r");
    if (!f)
    {
        snprintf(err_buf, err_buf_len, "cannot open token file: %s", path);
        return -1;
    }

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f))
    {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;

        // parse token and owner, then store in the table
        char token[AUTH_TOKEN_MAX_LEN];
        char owner[AUTH_OWNER_MAX_LEN];
        if (sscanf(p, "%127s %63s", token, owner) != 2)
        {
            snprintf(err_buf, err_buf_len, "%s:%d: expected '<token> <owner>'", path, lineno);
            fclose(f);
            return -1;
        }

        if (tbl->count >= AUTH_MAX_TOKENS)
        {
            snprintf(err_buf, err_buf_len, "%s: too many tokens (max %d)", path, AUTH_MAX_TOKENS);
            fclose(f);
            return -1;
        }

        struct auth_entry *e = &tbl->entries[tbl->count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->token, sizeof(e->token), "%s", token);
        snprintf(e->owner, sizeof(e->owner), "%s", owner);
    }
    fclose(f);

    if (tbl->count == 0)
    {
        snprintf(err_buf, err_buf_len, "%s: no tokens defined", path);
        return -1;
    }
    return 0;
}

int auth_check(const struct auth_table *tbl, const char *token, char *owner_out, size_t owner_len)
{
    char padded[AUTH_TOKEN_MAX_LEN];
    memset(padded, 0, sizeof(padded));
    snprintf(padded, sizeof(padded), "%s", token);

    int found = -1;
    for (int i = 0; i < tbl->count; i++)
    {
        if (fixed_time_eq(padded, tbl->entries[i].token))
            found = i;
    }
    if (found < 0)
        return -1;

    if (owner_out)
        snprintf(owner_out, owner_len, "%s", tbl->entries[found].owner);
    return 0;
}
