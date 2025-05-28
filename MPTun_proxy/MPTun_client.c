#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <signal.h>

#define BUF_SIZE 4096
const char *g_dns_ip = NULL;
const char *g_tun_name = NULL;

void cleanup(int signo) {
    char cmd[256];

    if (g_dns_ip && g_tun_name) {
        // Restore resolv.conf
        system("cp /etc/resolv.conf.backup /etc/resolv.conf");
        system("rm -f /etc/resolv.conf.backup");

        // Remove static route for DNS
        snprintf(cmd, sizeof(cmd), "ip route del %s dev %s", g_dns_ip, g_tun_name);
        system(cmd);

        // Remove default route through TUN
        snprintf(cmd, sizeof(cmd), "ip route del default dev %s", g_tun_name);
        system(cmd);

        // Bring down TUN
        snprintf(cmd, sizeof(cmd), "ip link set %s down", g_tun_name);
        system(cmd);
    }
    if (signo==0)
        printf("[*] Cleanup triggered. Exiting.\n");
    else
        printf("[*] Cleanup triggered by signal %d. Exiting.\n", signo);
    exit(0);
}


void setup_signal_handlers() {
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
}

// Create a TUN device
int tun_alloc(char *dev) {
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("[-] open(/dev/net/tun)");
        exit(1);
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("[-] ioctl(TUNSETIFF)");
        close(fd);
        exit(1);
    }

    // SUCCESS
    printf("[*] TUN interface actually created: %s\n", ifr.ifr_name);
    strcpy(dev, ifr.ifr_name);
    return fd;
}

// Connect to server using TCP (MPTCP-enabled kernel handles subflows)
int tcp_connect(const char *server_ip, int port,
                char *assigned_ip, size_t ip_len,
                char *dns_ip, size_t dns_ip_len) {
    int sock;
    struct sockaddr_in server;

    if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_MPTCP)) < 0) {
        perror("Creating socket");
        exit(1);
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &server.sin_addr);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("Connect failed");
        close(sock);
        exit(1);
    }

    // Read both IP and DNS IP
    char buffer[128];
    ssize_t n = read(sock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        perror("Failed to read IP/DNS info");
        close(sock);
        exit(1);
    }
    buffer[n] = '\0';

    // Parse lines
    char *line1 = strtok(buffer, "\n");
    char *line2 = strtok(NULL, "\n");

    if (!line1 || !line2) {
        fprintf(stderr, "[-] Invalid setup message format received from server\n");
        close(sock);
        exit(1);
    }

    strncpy(assigned_ip, line1, ip_len - 1);
    assigned_ip[ip_len - 1] = '\0';

    strncpy(dns_ip, line2, dns_ip_len - 1);
    dns_ip[dns_ip_len - 1] = '\0';

    printf("[*] Assigned IP: %s\n", assigned_ip);
    printf("[*] DNS Server:  %s\n", dns_ip);

    return sock;
}

int read_framed_packet(int fd, char *buf, int maxlen) {
    uint8_t hdr[2];
    int ret = 0, read_hdr = 0;

    // Read exactly 2 bytes for header
    while (read_hdr < 2) {
        ret = read(fd, hdr + read_hdr, 2 - read_hdr);
        if (ret == 0) {
            fprintf(stderr, "[DEBUG] read_framed_packet: EOF while reading length header (fd=%d)\n", fd);
            return -1;
        } else if (ret < 0) {
            fprintf(stderr, "[DEBUG] read_framed_packet: Error reading length header (fd=%d): %s (errno %d)\n",
                    fd, strerror(errno), errno);
            return -1;
        }
        read_hdr += ret;
    }

    // Safely construct length
    uint16_t len = (hdr[0] << 8) | hdr[1];

    if (len > maxlen) {
        fprintf(stderr, "[WARN] read_framed_packet: Length %u exceeds maxlen %d. Dropping packet.\n", len, maxlen);
        return 0;
    }
    if (len < 1) {
        fprintf(stderr, "[WARN] Received 0-byte packet, skipping write to tun_fd.\n");
        return 0;
    }

    int received = 0;
    while (received < len) {
        int n = read(fd, buf + received, len - received);
        if (n == 0) {
            fprintf(stderr, "[DEBUG] read_framed_packet: EOF while reading packet body (fd=%d, received=%d/%d)\n",
                    fd, received, len);
            return -1;
        } else if (n < 0) {
            fprintf(stderr, "[DEBUG] read_framed_packet: Error reading packet body (fd=%d): %s (errno %d)\n",
                    fd, strerror(errno), errno);
            return -1;
        }

        received += n;
    }

    return len;
}

// int send_framed_packet(int fd, const char *buf, int len) {
//     uint16_t hdr = htons(len);
//     if (write(fd, &hdr, 2) != 2) return -1;
//     if (write(fd, buf, len) != len) return -1;
//     return 0;
// }

int send_framed_packet(int fd, const char *buf, int len) {
    uint16_t hdr = htons(len);
    
    // Debug: Print header bytes
    printf("[DEBUG] Sending packet: length = %d (0x%04x)\n", len, len);
    printf("[DEBUG] Header bytes: %02x %02x\n", ((unsigned char*)&hdr)[0], ((unsigned char*)&hdr)[1]);

    // Optional: Print first 16 bytes of payload
    printf("[DEBUG] First 16 bytes of payload: ");
    for (int i = 0; i < 16 && i < len; ++i) {
        printf("%02x ", (unsigned char)buf[i]);
    }
    printf("\n");

    if (write(fd, &hdr, 2) != 2) return -1;
    if (write(fd, buf, len) != len) return -1;
    return 0;
}


int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <tun_name> <gateway_ip> <physical_iface>\n", argv[0]);
        exit(1);
    }

    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    char tun_name[IFNAMSIZ];
    strncpy(tun_name, argv[3], IFNAMSIZ - 1);
    tun_name[IFNAMSIZ - 1] = '\0';

    const char *gateway_ip = argv[4];
    const char *physical_if = argv[5];

    // Require root
    if (geteuid() != 0) {
        fprintf(stderr, "[-] This program must be run as root.\n");
        exit(1);
    }

    char assigned_ip[32];
    char dns_ip[32];
    int sock_fd = tcp_connect(server_ip, server_port, assigned_ip, sizeof(assigned_ip), dns_ip, sizeof(dns_ip));

    // Setup routing
    int tun_fd = tun_alloc(tun_name);

    char cmd[256];

    // 1. Assign TUN IP address
    snprintf(cmd, sizeof(cmd), "ip addr add %s/24 dev %s", assigned_ip, tun_name);
    system(cmd);

    // 2. Bring up TUN interface
    snprintf(cmd, sizeof(cmd), "ip link set %s up", tun_name);
    system(cmd);

    // 3. Ensure proxy traffic (to server_ip) uses physical interface
    snprintf(cmd, sizeof(cmd), "ip route replace %s via %s dev %s", server_ip, gateway_ip, physical_if);
    system(cmd);

    // 4. Replace default route to send all other traffic via TUN
    snprintf(cmd, sizeof(cmd), "ip route replace default dev %s", tun_name);
    system(cmd);

    // 5. Set DNS server by updating resolv.conf
    system("cp /etc/resolv.conf /etc/resolv.conf.backup");
    FILE *resolv = fopen("/etc/resolv.conf", "w");
    if (resolv) {
        fprintf(resolv, "nameserver %s\n", dns_ip);
        fclose(resolv);
        printf("[*] DNS server set to %s\n", dns_ip);
    } else {
        perror("[-] Failed to write /etc/resolv.conf");
    }
    snprintf(cmd, sizeof(cmd), "ip route replace %s dev %s", dns_ip, tun_name);
    system(cmd);

    

    char buffer[BUF_SIZE];
    printf("[*] TUN device '%s' created and MPTCP (or TCP) connection established to %s:%d.\n", tun_name, server_ip, server_port);

    g_dns_ip = dns_ip;
    g_tun_name = tun_name;
    setup_signal_handlers();

    while (1) {
        memset(buffer, 0, BUF_SIZE);
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);
        FD_SET(sock_fd, &readfds);
        int maxfd = (tun_fd > sock_fd) ? tun_fd : sock_fd;

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select()");
            cleanup(0);
            exit(1);
        }

        // TUN → Proxy
        if (FD_ISSET(tun_fd, &readfds)) {
            int nread = read(tun_fd, buffer, BUF_SIZE);
            if (nread > 0) {
                if (send_framed_packet(sock_fd, buffer, nread) < 0) {
                    perror("send_framed_packet()");
                }
            }
        }

        // Proxy → TUN
        if (FD_ISSET(sock_fd, &readfds)) {
            int n = read_framed_packet(sock_fd, buffer, BUF_SIZE);
            if (n > 0) {
                write(tun_fd, buffer, n);
            } else {
                printf("[*] Server closed connection or framing error\n");
                break;
            }
        }
    }

    close(tun_fd);
    close(sock_fd);
    cleanup(0);
    return 0;
}
