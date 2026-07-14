#ifndef SERVER_H
#define SERVER_H

struct registry;

// Binds a UNIX domain socket at sock_path and serves the control-plane
// protocol forever. Returns -1 on bind/listen failure; otherwise never returns.
int server_run(const char *sock_path, struct registry *reg);

#endif // SERVER_H
