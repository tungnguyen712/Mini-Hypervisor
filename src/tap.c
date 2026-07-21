#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/if.h>
#include <linux/if_tun.h>

int tap_octet_for_vm(int vm_id)
{
    return ((vm_id - 1) % 254) + 1;
}

void tap_ifname_for_vm(int vm_id, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "tap%d", vm_id);
}

void tap_host_ip_for_vm(int vm_id, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "172.30.%d.1", tap_octet_for_vm(vm_id));
}

void tap_guest_ip_for_vm(int vm_id, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "172.30.%d.2", tap_octet_for_vm(vm_id));
}

void tap_mac_for_vm(int vm_id, uint8_t mac[6])
{
    mac[0] = 0x52;
    mac[1] = 0x54;
    mac[2] = 0x00;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = (uint8_t)tap_octet_for_vm(vm_id);
}

static int run_cmd(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        return -1;
    }
    if (pid == 0)
    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0)
    {
        perror("waitpid");
        return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int tap_create(int vm_id, char *ifname_out, size_t ifname_len)
{
    tap_ifname_for_vm(vm_id, ifname_out, ifname_len);

    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0)
    {
        perror("open /dev/net/tun");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname_out);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0)
    {
        perror("TUNSETIFF");
        close(fd);
        return -1;
    }
    return fd;
}

int tap_host_configure(const char *ifname, int vm_id)
{
    char host_ip[32];
    tap_host_ip_for_vm(vm_id, host_ip, sizeof(host_ip));
    char cidr[48];
    snprintf(cidr, sizeof(cidr), "%s/24", host_ip);

    char *addr_argv[] = {"ip", "addr", "add", cidr, "dev", (char *)ifname, NULL};
    if (run_cmd(addr_argv) != 0)
    {
        fprintf(stderr, "tap_host_configure: 'ip addr add %s dev %s' failed\n", cidr, ifname);
        return -1;
    }

    char *up_argv[] = {"ip", "link", "set", (char *)ifname, "up", NULL};
    if (run_cmd(up_argv) != 0)
    {
        fprintf(stderr, "tap_host_configure: 'ip link set %s up' failed\n", ifname);
        return -1;
    }
    return 0;
}

// Finds the outbound interface of the host's default route
// ("ip route show default" -> "default via <gw> dev <ifname> ...")
static int detect_uplink_ifname(char *buf, size_t buflen)
{
    FILE *f = popen("ip route show default 2>/dev/null", "r");
    if (!f)
        return -1;

    char line[512];
    int found = -1;
    if (fgets(line, sizeof(line), f))
    {
        char *dev = strstr(line, " dev ");
        if (dev)
        {
            dev += 5;
            char ifname[IFNAMSIZ] = {0};
            if (sscanf(dev, "%15s", ifname) == 1)
            {
                snprintf(buf, buflen, "%s", ifname);
                found = 0;
            }
        }
    }
    pclose(f);
    return found;
}

int tap_isolate_vm(const char *ifname)
{
    char uplink[IFNAMSIZ];
    if (detect_uplink_ifname(uplink, sizeof(uplink)) != 0)
    {
        fprintf(stderr, "tap_isolate_vm: could not detect uplink interface;"
                        " %s will have no route out at all\n",
                ifname);
        return -1;
    }

    char *accept_uplink_argv[] = {"iptables", "-A", "FORWARD",
                                  "-i", (char *)ifname, "-o", uplink, "-j", "ACCEPT", NULL};
    if (run_cmd(accept_uplink_argv) != 0)
    {
        fprintf(stderr, "tap_isolate_vm: failed to add uplink ACCEPT rule for %s\n", ifname);
        return -1;
    }

    char *drop_rest_argv[] = {"iptables", "-A", "FORWARD", "-i", (char *)ifname, "-j", "DROP", NULL};
    if (run_cmd(drop_rest_argv) != 0)
    {
        fprintf(stderr, "tap_isolate_vm: failed to add isolation DROP rule for %s\n", ifname);
        char *undo_argv[] = {"iptables", "-D", "FORWARD",
                             "-i", (char *)ifname, "-o", uplink, "-j", "ACCEPT", NULL};
        run_cmd(undo_argv);
        return -1;
    }
    return 0;
}

int tap_ensure_nat(void)
{
    char *fwd_argv[] = {"sysctl", "-w", "net.ipv4.ip_forward=1", NULL};
    if (run_cmd(fwd_argv) != 0)
    {
        fprintf(stderr, "tap_ensure_nat: failed to enable net.ipv4.ip_forward\n");
        return -1;
    }

    char uplink[IFNAMSIZ];
    if (detect_uplink_ifname(uplink, sizeof(uplink)) != 0)
    {
        fprintf(stderr, "tap_ensure_nat: could not detect a default route interface;"
                        " guests will get a tap link but no NAT to the internet\n");
        return -1;
    }

    char *check_argv[] = {"iptables", "-t", "nat", "-C", "POSTROUTING",
                          "-s", "172.30.0.0/16", "-o", uplink, "-j", "MASQUERADE", NULL};
    if (run_cmd(check_argv) == 0)
        return 0; // already present

    char *add_argv[] = {"iptables", "-t", "nat", "-A", "POSTROUTING",
                        "-s", "172.30.0.0/16", "-o", uplink, "-j", "MASQUERADE", NULL};
    if (run_cmd(add_argv) != 0)
    {
        fprintf(stderr, "tap_ensure_nat: failed to add MASQUERADE rule for %s\n", uplink);
        return -1;
    }
    return 0;
}

int tap_add_forward(int vm_id, int host_port, int guest_port)
{
    char guest_ip[32];
    tap_guest_ip_for_vm(vm_id, guest_ip, sizeof(guest_ip));
    char host_port_s[16], guest_port_s[16], to_dest[48];
    snprintf(host_port_s, sizeof(host_port_s), "%d", host_port);
    snprintf(guest_port_s, sizeof(guest_port_s), "%d", guest_port);
    snprintf(to_dest, sizeof(to_dest), "%s:%d", guest_ip, guest_port);

    char *dnat_argv[] = {"iptables", "-t", "nat", "-A", "PREROUTING",
                         "-p", "tcp", "--dport", host_port_s,
                         "-j", "DNAT", "--to", to_dest, NULL};
    if (run_cmd(dnat_argv) != 0)
    {
        fprintf(stderr, "tap_add_forward: DNAT rule failed for host_port=%d\n", host_port);
        return -1;
    }

    char *accept_argv[] = {"iptables", "-A", "FORWARD", "-p", "tcp",
                           "-d", guest_ip, "--dport", guest_port_s, "-j", "ACCEPT", NULL};
    if (run_cmd(accept_argv) != 0)
    {
        fprintf(stderr, "tap_add_forward: FORWARD accept rule failed for guest_port=%d\n", guest_port);
        // Roll back the DNAT rule we just added so failure doesn't leave a
        // half-applied forward.
        char *undo_argv[] = {"iptables", "-t", "nat", "-D", "PREROUTING",
                             "-p", "tcp", "--dport", host_port_s,
                             "-j", "DNAT", "--to", to_dest, NULL};
        run_cmd(undo_argv);
        return -1;
    }
    return 0;
}

static int delete_matching_rules(const char *table, const char *chain, const char *match_pattern)
{
    char cmd[400];
    snprintf(cmd, sizeof(cmd),
             "iptables -t %s -L %s -n -v --line-numbers 2>/dev/null | "
             "awk '%s{print $1}' | sort -rn",
             table, chain, match_pattern);
    FILE *f = popen(cmd, "r");
    if (!f)
        return -1;
    char line[32];
    int removed_any = 0;
    while (fgets(line, sizeof(line), f))
    {
        int rule_no = atoi(line);
        if (rule_no <= 0)
            continue;
        char rule_no_s[16];
        snprintf(rule_no_s, sizeof(rule_no_s), "%d", rule_no);
        char *del_argv[] = {"iptables", "-t", (char *)table, "-D", (char *)chain, rule_no_s, NULL};
        if (run_cmd(del_argv) == 0)
            removed_any = 1;
    }
    pclose(f);
    return removed_any;
}

int tap_remove_forward(int vm_id, int host_port)
{
    char guest_ip[32];
    tap_guest_ip_for_vm(vm_id, guest_ip, sizeof(guest_ip));
    char host_port_s[16];
    snprintf(host_port_s, sizeof(host_port_s), "%d", host_port);

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "$0 ~ /dpt:%s / && $0 ~ /to:%s/", host_port_s, guest_ip);
    int removed_prerouting = delete_matching_rules("nat", "PREROUTING", pattern);

    char ip_pattern[96];
    snprintf(ip_pattern, sizeof(ip_pattern), "$0 ~ /%s/", guest_ip);
    delete_matching_rules("filter", "FORWARD", ip_pattern); // best-effort accept-rule cleanup

    return removed_prerouting ? 0 : -1;
}

int tap_remove_isolation(const char *ifname)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "$7 == \"%s\"", ifname);
    return delete_matching_rules("filter", "FORWARD", pattern) ? 0 : -1;
}