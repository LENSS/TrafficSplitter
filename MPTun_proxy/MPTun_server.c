#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <net/if.h>

#define BUF_SIZE 4096
#define MAX_CLIENTS 256

typedef struct {
    pid_t pid;
    char ip[32];
} client_info_t;

client_info_t client_table[MAX_CLIENTS];
int client_count = 0;

int ip_pool[253] = {0}; // 10.0.1.2 ~ 10.0.1.254

int allocate_ip(char *out_ip) {
    for (int i = 0; i < 253; ++i) {
        if (!ip_pool[i]) {
            ip_pool[i] = 1;
            snprintf(out_ip, 32, "10.0.1.%d", i + 2);
            //printf("[*] Assigned inner IP 10.0.1.%d (slot %d)\n", i + 2, i);
            return 0;
        }
    }
    return -1;
}

void release_ip(const char *ip) {
    int last_octet;
    if (sscanf(ip, "10.0.1.%d", &last_octet) == 1 && last_octet >= 2 && last_octet <= 254) {
        ip_pool[last_octet - 2] = 0;
        //printf("[*] Released inner IP %s (slot %d)\n", ip, last_octet - 2);
    } else {
        fprintf(stderr, "[-] Invalid IP to release: %s\n", ip);
    }
}

void sigchld_handler(int signo) {
    while (1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;

        for (int i = 0; i < client_count; ++i) {
            if (client_table[i].pid == pid) {
                release_ip(client_table[i].ip);
                printf("[*] Released inner IP %s (PID %d)\n", client_table[i].ip, pid);
                client_table[i] = client_table[--client_count];
                break;
            }
        }
    }
}

int tun_alloc(char *dev) {
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        exit(1);
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("ioctl(TUNSETIFF)");
        close(fd);
        exit(1);
    }

    printf("[*] TUN interface created: %s\n", ifr.ifr_name);
    return fd;
}

int read_framed_packet(int fd, char *buf, int maxlen) {
    uint16_t len;
    int ret = read(fd, &len, 2);
    if (ret <= 0) return -1;
    len = ntohs(len);

    int received = 0;
    while (received < len) {
        int n = read(fd, buf + received, len - received);
        if (n <= 0) return -1;
        received += n;
    }
    return len;
}

int send_framed_packet(int fd, const char *buf, int len) {
    uint16_t hdr = htons(len);
    if (write(fd, &hdr, 2) != 2) return -1;
    if (write(fd, buf, len) != len) return -1;
    return 0;
}

void handle_client(int client_fd, int tun_fd, char *client_ip) {
    char buffer[BUF_SIZE];

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        FD_SET(tun_fd, &readfds);
        int maxfd = (client_fd > tun_fd) ? client_fd : tun_fd;

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select()");
            break;
        }

        if (FD_ISSET(client_fd, &readfds)) {
            int n = read_framed_packet(client_fd, buffer, BUF_SIZE);
            if (n <= 0) {
                printf("[*] Client disconnected.\n");
                break;
            }
            if (write(tun_fd, buffer, n) < 0) {
                perror("write(tun_fd)");
            }
        }

        if (FD_ISSET(tun_fd, &readfds)) {
            int n = read(tun_fd, buffer, BUF_SIZE);
            if (n > 0) {
                if (send_framed_packet(client_fd, buffer, n) < 0) {
                    perror("send_framed_packet()");
                }
            }
        }
    }
    close(client_fd);
    close(tun_fd);
    exit(0);
}
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <bind_ip> <port> <physical_interface>\n", argv[0]);
        exit(1);
    }

    const char *bind_ip = argv[1];
    int port = atoi(argv[2]);
    const char *physical_if = argv[3];

    char tun_name[IFNAMSIZ] = "mptcp_tun";
    int tun_fd = tun_alloc(tun_name);

    char cmd[256];

    // Bring up TUN interface
    snprintf(cmd, sizeof(cmd), "ip link set %s up", tun_name);
    system(cmd);

    // Assign IP address to TUN interface
    snprintf(cmd, sizeof(cmd), "ip addr add 10.0.1.1/24 dev %s 2>/dev/null", tun_name);
    system(cmd);

    // Enable IP forwarding (safe to call multiple times)
    system("sysctl -w net.ipv4.ip_forward=1");

    // Setup NAT for outbound traffic using specified physical interface
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -C POSTROUTING -o %s -j MASQUERADE 2>/dev/null || "
             "iptables -t nat -A POSTROUTING -o %s -j MASQUERADE",
             physical_if, physical_if);
    system(cmd);


    int listen_fd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    signal(SIGCHLD, sigchld_handler);

    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_MPTCP);
    if (listen_fd < 0) {
        perror("socket()");
        exit(1);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &serv_addr.sin_addr) != 1) {
        perror("inet_pton()");
        exit(1);
    }

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind()");
        exit(1);
    }

    if (listen(listen_fd, 10) < 0) {
        perror("listen()");
        exit(1);
    }

    printf("[*] MPTCP tunnel server listening on %s:%d...\n", bind_ip, port);

    while (1) {
        int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_fd < 0) {
            perror("accept()");
            continue;
        }

        printf("[*] New client from %s\n", inet_ntoa(cli_addr.sin_addr));

        char assigned_ip[32];
        if (allocate_ip(assigned_ip) < 0) {
            fprintf(stderr, "[-] No available IP addresses\n");
            close(client_fd);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork()");
            release_ip(assigned_ip);
            close(client_fd);
        } else if (pid == 0) {
            // Child
            close(listen_fd);
            write(client_fd, assigned_ip, strlen(assigned_ip));
            handle_client(client_fd, tun_fd, assigned_ip);
        } else {
            // Parent
            printf("[*] Inner IP (%s) assigned to client (%s), handled by child PID %d\n",
                assigned_ip, inet_ntoa(cli_addr.sin_addr), pid);

            if (client_count < MAX_CLIENTS) {
                client_table[client_count].pid = pid;
                strncpy(client_table[client_count].ip, assigned_ip, sizeof(client_table[client_count].ip));
                client_count++;
            }
            close(client_fd);
        }
    }

    return 0;
}