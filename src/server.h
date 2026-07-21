#ifndef SERVER_H
#define SERVER_H

struct registry;
struct auth_table;

// Binds a UNIX domain socket at sock_path and serves the control-plane
// protocol forever. Every connection must send "AUTH <token>" (checked
// against auth) before any other command is accepted
int server_run(const char *sock_path, struct registry *reg,
               const struct auth_table *auth, const char *images_dir);

#endif // SERVER_H
