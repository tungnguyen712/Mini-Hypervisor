#ifndef TAP_H
#define TAP_H

#include <stddef.h>
#include <stdint.h>

// Deterministic per-VM addressing: subnet 172.30.<octet>.<1/2>/24,
// for <octet>, host side is .1, guest side is .2.

// convert VM id into 3rd octet of 172.30.<octet>.1/24
int tap_octet_for_vm(int vm_id);
// generate tap interface name
void tap_ifname_for_vm(int vm_id, char *buf, size_t buflen);
// generate ip address assigned to host side of TAP network
void tap_host_ip_for_vm(int vm_id, char *buf, size_t buflen);
// generate ip address assigned to guest VM
void tap_guest_ip_for_vm(int vm_id, char *buf, size_t buflen);
// generate MAC address for each vm
void tap_mac_for_vm(int vm_id, uint8_t mac[6]);

// Opens /dev/net/tun and creates a TAP interface named tap<id>
int tap_create(int vm_id, char *ifname_out, size_t ifname_len);

// ip addr add <host-ip>/24 dev <ifname>; ip link set <ifname> up
int tap_host_configure(const char *ifname, int vm_id);

// isolate tap between VMs
int tap_isolate_vm(const char *ifname);
int tap_remove_isolation(const char *ifname);

// enable ipv4 forwarding and add MASQUERADE rule
// call once at daemon startup, not per VM
int tap_ensure_nat(void);

// forward a port on the host to a port inside one VM
// iptables DNAT: host_port -> this VM's guest IP:guest_port.
int tap_add_forward(int vm_id, int host_port, int guest_port);
int tap_remove_forward(int vm_id, int host_port);

#endif // TAP_H