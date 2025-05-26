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

#define BUF_SIZE 4096

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
int tcp_connect(const char *server_ip, int port, char *assigned_ip, size_t ip_len) {
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

    // Read assigned IP
    ssize_t n = read(sock, assigned_ip, ip_len - 1);
    if (n <= 0) {
        perror("Failed to read assigned IP");
        close(sock);
        exit(1);
    }

    // Null-terminate and strip newline if present
    assigned_ip[n] = '\0';
    char *newline = strchr(assigned_ip, '\n');
    if (newline) *newline = '\0';

    return sock;
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
    int sock_fd = tcp_connect(server_ip, server_port, assigned_ip, sizeof(assigned_ip));

    // Setup routing
    int tun_fd = tun_alloc(tun_name);

    char cmd[256];

    // 1. Assign TUN IP address (OpenVPN style)
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


    char buffer[BUF_SIZE];
    printf("[*] TUN device '%s' created and MPTCP (or TCP) connection established to %s:%d.\n", tun_name, server_ip, server_port);

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);
        FD_SET(sock_fd, &readfds);
        int maxfd = (tun_fd > sock_fd) ? tun_fd : sock_fd;

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select()");
            exit(1);
        }

        // TUN → Proxy (Send packet with 2-byte length prefix)
        if (FD_ISSET(tun_fd, &readfds)) {
            int nread = read(tun_fd, buffer + 2, BUF_SIZE - 2);
            if (nread > 0) {
                uint16_t len = htons(nread);
                memcpy(buffer, &len, 2);
                if (write(sock_fd, buffer, nread + 2) < 0) {
                    perror("write(sock_fd)");
                }
            }
        }

        // Proxy → TUN (Receive full framed packets)
        if (FD_ISSET(sock_fd, &readfds)) {
            // Step 1: Read 2-byte length prefix
            uint16_t pkt_len;
            int ret = read(sock_fd, &pkt_len, 2);
            if (ret <= 0) {
                printf("[*] Server closed connection (length)\n");
                break;
            }

            pkt_len = ntohs(pkt_len);
            int received = 0;
            while (received < pkt_len) { // here can be error? I am not sure lets check if we have an issue.
                int n = read(sock_fd, buffer + received, pkt_len - received);
                if (n <= 0) {
                    printf("[*] Server closed connection (body)\n");
                    break;
                }
                received += n;
            }

            if (write(tun_fd, buffer, pkt_len) < 0) {
                perror("write(tun_fd)");
            }
        }
    }

    close(tun_fd);
    close(sock_fd);
    return 0;
}