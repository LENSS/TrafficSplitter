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
#include <netinet/tcp.h>
#include <sys/time.h>
#include <poll.h>
#include <time.h>

// === Logging Levels ===
#define LOG_ERROR 0
#define LOG_WARN  1
#define LOG_INFO  2
#define LOG_DEBUG 3
#define LOG_TRACE 4

// === Default log level (can override via env or CLI) ===
int LOG_LEVEL = LOG_INFO;

// === Optional color output for log levels ===
#define COLOR_RESET "\033[0m"
#define COLOR_RED   "\033[0;31m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_GREEN "\033[0;32m"
#define COLOR_CYAN  "\033[0;36m"
#define COLOR_GRAY  "\033[1;30m"

// === Logging macro ===
#define LOG(level, fmt, ...) \
    do { \
        if ((level) <= LOG_LEVEL) { \
            const char *color = ""; \
            const char *level_str = ""; \
            if (level == LOG_ERROR) { color = COLOR_RED; level_str = "ERROR"; } \
            else if (level == LOG_WARN) { color = COLOR_YELLOW; level_str = "WARN"; } \
            else if (level == LOG_INFO) { color = COLOR_GREEN; level_str = "INFO"; } \
            else if (level == LOG_DEBUG) { color = COLOR_CYAN; level_str = "DEBUG"; } \
            else if (level == LOG_TRACE) { color = COLOR_GRAY; level_str = "TRACE"; } \
            fprintf(stderr, "%s[%s] " fmt COLOR_RESET "\n", color, level_str, ##__VA_ARGS__); \
        } \
    } while (0)

#define BUF_SIZE 16384
#define MAX_CLIENTS 128
#define MERGE_TARGET_SIZE 3000
#define MERGE_TIME_WINDOW_US  10000 

char merge_buf[MERGE_TARGET_SIZE*3];
int merge_len = 0;

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
    } else {
        LOG(LOG_WARN, "Invalid IP to release: %s", ip);
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
                LOG(LOG_INFO, "Released inner IP %s (PID %d)", client_table[i].ip, pid);
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
    LOG(LOG_INFO, "[*] TUN interface created: %s", ifr.ifr_name);
    return fd;
}

int read_framed_packet(int fd, char *buf, int maxlen) {
    uint8_t hdr[2];
    int ret = 0, read_hdr = 0;
    // Read exactly 2 bytes for header
    while (read_hdr < 2) {
        ret = read(fd, hdr + read_hdr, 2 - read_hdr);
        if (ret == 0) {
            LOG(LOG_DEBUG, "read_framed_packet: EOF while reading length header (fd=%d)", fd);
            return -1;
        } else if (ret < 0) {
            LOG(LOG_DEBUG, "read_framed_packet: Error reading length header (fd=%d): %s (errno %d)",
                    fd, strerror(errno), errno);
            return -1;
        }
        read_hdr += ret;
    }
    // Safely construct length
    uint16_t len = (hdr[0] << 8) | hdr[1];

    if (len > maxlen) {
        LOG(LOG_WARN, "read_framed_packet: Length %u exceeds maxlen %d. Dropping packet.", len, maxlen);
        return 0;
    }
    if (len < 1) {
        LOG(LOG_WARN, "Received 0-byte packet, skipping write to tun_fd.");
        return 0;
    }
    int received = 0;
    while (received < len) {
        int n = read(fd, buf + received, len - received);
        if (n == 0) {
            LOG(LOG_DEBUG, "read_framed_packet: EOF while reading packet body (fd=%d, received=%d/%d)",
                    fd, received, len);
            return -1;
        } else if (n < 0) {
            LOG(LOG_DEBUG, "read_framed_packet: Error reading packet body (fd=%d): %s (errno %d)",
                    fd, strerror(errno), errno);
            return -1;
        }
        received += n;
    }
    return len;
}

void handle_client(int client_fd, int tun_fd, char *client_ip) {
    char buffer[BUF_SIZE];
    struct timeval merge_time_limit;
    gettimeofday(&merge_time_limit, NULL);

    struct pollfd fds[2];
    fds[0].fd = client_fd;
    fds[0].events = POLLIN;
    fds[1].fd = tun_fd;
    fds[1].events = POLLIN;
    float burst_bkt = 0;

    while (1) {
        memset(buffer, 0, BUF_SIZE);

        int ret = poll(fds, 2, -1);  // -1 means wait indefinitely
        if (ret < 0) {
            perror("poll()");
            break;
        }
        ////////////////////////////////////////////////
        // Check merger buffer to flush ////////////////
        ////////////////////////////////////////////////
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_us = (now.tv_sec - merge_time_limit.tv_sec) * 1000000L +
                        (now.tv_usec - merge_time_limit.tv_usec);
        if (elapsed_us >= MERGE_TIME_WINDOW_US || merge_len >= MERGE_TARGET_SIZE) {  // Need to update "merge_time_limit" and flush merge_buf if needed
            merge_time_limit = now;
            // If there is remaining data in merged_buf, flush it.
            if (merge_len > 0) {
                if (write(client_fd, merge_buf, merge_len) < 0) {
                    LOG(LOG_ERROR, "Fail to flush merged_buf write() at the start of time window (TUN -> Client)");
                } else {
                    LOG(LOG_DEBUG,"Flushed %d bytes to client at the start of time window (TUN -> Client)", merge_len);
                    merge_len = 0;
                }
            }
        }
        /////////////////////////////////////////////
        // === TUN -> CLIENT (merge) === ////////////
        /////////////////////////////////////////////
        if (fds[1].revents & POLLIN) {
            int nread = read(tun_fd, buffer, BUF_SIZE);
            if (nread > 0) {
                LOG(LOG_DEBUG,"Read %d bytes from TUN", nread);
                //Sanity check on merge buffer size
                if (merge_len + nread + 2 > MERGE_TARGET_SIZE) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    long elapsed_us = (now.tv_sec - merge_time_limit.tv_sec) * 1000000L +
                                    (now.tv_usec - merge_time_limit.tv_usec);    
                    merge_time_limit = now;
                    if (write(client_fd, merge_buf, merge_len) < 0) {
                        LOG(LOG_ERROR, "Fail to flush merged_buf write() at the start of time window (TUN -> Client)");
                    } else {
                        LOG(LOG_DEBUG,"Flushed %d bytes to client at the start of time window (TUN -> Client)", merge_len);
                        merge_len = 0;
                    }
                }
                // Prepend packet length (network order)
                LOG(LOG_DEBUG,"Prepending length header and copying packet...");
                merge_buf[merge_len] = (nread >> 8) & 0xFF;
                merge_buf[merge_len + 1] = nread & 0xFF;
                memcpy(merge_buf + merge_len + 2, buffer, nread);
                merge_len += nread + 2;
                LOG(LOG_DEBUG,"Appended packet of %d bytes (merge_len now %d)", nread, merge_len);
            } else if (nread < 0) {
                LOG(LOG_ERROR, "TUN read()");
            } else {
                LOG(LOG_DEBUG,"TUN read() returned 0 (EOF?)");
            }
        }
        /////////////////////////////////////////////
        // === CLIENT -> TUN (no merge needed) === //
        /////////////////////////////////////////////
        if (fds[0].revents & POLLIN) {
            LOG(LOG_TRACE, "client_fd is readable");

            int n = read_framed_packet(client_fd, buffer, BUF_SIZE);
            LOG(LOG_TRACE, "read_framed_packet returned %d", n);

            if (n < 0) {
                LOG(LOG_INFO, "Client disconnected or error in framing.");
                break;
            } else if (n == 0) {
                LOG(LOG_TRACE, "No complete framed packet available (n == 0)");
            } else {
                LOG(LOG_TRACE, "Writing %d bytes to tun_fd=%d", n, tun_fd);
                int w = write(tun_fd, buffer, n);
                if (w < 0) {
                    LOG(LOG_ERROR, "write(tun_fd) failed: Tried writing %d bytes to tun_fd=%d", n, tun_fd);
                    perror("write(tun_fd)");
                } else {
                    LOG(LOG_DEBUG,"Successfully wrote %d bytes to tun_fd", w);
                }
            }
        }
    }

    close(client_fd);
    close(tun_fd);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <bind_ip> <port> <physical_interface> <dns_ip>\n", argv[0]);
        exit(1);
    }

    // Set LOG_LEVEL from environment variable "LOGLEVEL", if provided
    char *lvl_env = getenv("LOGLEVEL");
    if (lvl_env) {
        int lvl = atoi(lvl_env);
        if (lvl >= LOG_ERROR && lvl <= LOG_TRACE) {
            LOG_LEVEL = lvl;
        }
    } // HOW TO USE? -> LOGLEVEL=4 ./client   

    const char *bind_ip = argv[1];
    int port = atoi(argv[2]);
    const char *physical_if = argv[3];
    const char *dns_ip = argv[4];

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

    LOG(LOG_INFO,  "MPTCP tunnel server listening on %s:%d...", bind_ip, port);

    while (1) {
        socklen_t cli_len = sizeof(cli_addr);  // ensure cli_len is reset each time
        int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_fd < 0) {
            LOG(LOG_ERROR, "accept()");
            continue;
        }

        // Optional: disable Nagle for latency-sensitive traffic
        int flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            perror("setsockopt(TCP_NODELAY)");
        }

        // Set TCP_NOTSENT_LOWAT for bufferbloat control
        unsigned int lowat = 128 * 1024;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT,
                    &lowat, sizeof(lowat)) < 0) {
            perror("setsockopt(TCP_NOTSENT_LOWAT)");
        }

        // Avoid sending more than 1024 bytes at a time
        // int lowat = 2024;
        // setsockopt(client_fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));

        // // Keep total send buffer small (but not too small)
        // int sndbuf = 20480;
        // setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        LOG(LOG_INFO,  "New client from %s", inet_ntoa(cli_addr.sin_addr));
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
            char msg[64];
            snprintf(msg, sizeof(msg), "%s\n%s\n", assigned_ip, dns_ip);  // Client IP + DNS
            if (write(client_fd, msg, strlen(msg)) < 0) {
                perror("write(client_fd)");
                release_ip(assigned_ip);
                close(client_fd);
                exit(1);
            }
            LOG(LOG_INFO,  "Inner IP (%s) and DNS (%s) sent to client (%s)", assigned_ip, dns_ip, inet_ntoa(cli_addr.sin_addr));
            handle_client(client_fd, tun_fd, assigned_ip);
        } else {
            // Parent
            LOG(LOG_INFO,  "Inner IP (%s) assigned to client (%s), handled by child PID %d",
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
