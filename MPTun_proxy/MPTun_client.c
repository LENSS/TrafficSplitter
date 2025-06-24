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
#include <poll.h>
#include <sys/time.h>
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

// === MIN MAX for convenience ===
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

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

#define BUF_SIZE 65536
#define INIT_BYTES_PER_MS 1500.0

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
    LOG(LOG_INFO,  "TUN interface actually created: %s", ifr.ifr_name);
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
        LOG(LOG_ERROR, "Fail to create socket");
        exit(1);
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &server.sin_addr);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        LOG(LOG_ERROR, "Connect failed");
        close(sock);
        exit(1);
    }

    // Read both IP and DNS IP
    char buffer[128];
    ssize_t n = read(sock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        LOG(LOG_ERROR, "Failed to read IP/DNS info");
        close(sock);
        exit(1);
    }
    buffer[n] = '\0';

    // Parse lines
    char *line1 = strtok(buffer, "\n");
    char *line2 = strtok(NULL, "\n");

    if (!line1 || !line2) {
        LOG(LOG_WARN, "Invalid setup message format received from server\n");
        close(sock);
        exit(1);
    }

    strncpy(assigned_ip, line1, ip_len - 1);
    assigned_ip[ip_len - 1] = '\0';

    strncpy(dns_ip, line2, dns_ip_len - 1);
    dns_ip[dns_ip_len - 1] = '\0';

    LOG(LOG_INFO,  "Assigned Inner IP: %s", assigned_ip);
    LOG(LOG_INFO,  "Notified DNS Server:  %s", dns_ip);

    return sock;
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

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <tun_name> <gateway_ip> <physical_iface>\n", argv[0]);
        exit(1);
    }

    // Set LOG_LEVEL from environment variable "LOGLEVEL", if provided
    char *lvl_env = getenv("LOGLEVEL");
    if (lvl_env) {
        int lvl = atoi(lvl_env);
        if (lvl >= LOG_ERROR && lvl <= LOG_TRACE) {
            LOG_LEVEL = lvl;
        }
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
    snprintf(cmd, sizeof(cmd), "echo \"nameserver %s\" | tee /etc/resolv.conf", dns_ip);
    system(cmd);
    
    // FILE *resolv = fopen("/etc/resolv.conf", "w");
    // if (resolv) {
    //     fprintf(resolv, "nameserver %s\n", dns_ip);
    //     fclose(resolv);
    //     LOG(LOG_INFO,  "DNS server set to %s", dns_ip);
    // } else {
    //     LOG(LOG_WARN,  "Failed to write /etc/resolv.conf");
    // }
    snprintf(cmd, sizeof(cmd), "ip route replace %s dev %s", dns_ip, tun_name);
    system(cmd);

    LOG(LOG_INFO,  "TUN device '%s' created and MPTCP (or TCP) connection established to %s:%d.", tun_name, server_ip, server_port);

    g_dns_ip = dns_ip;
    g_tun_name = tun_name;
    setup_signal_handlers();

    char buffer[BUF_SIZE]; // buffer for general read() and write()

    struct pollfd fds[2];
    fds[0].fd = tun_fd;
    fds[0].events = POLLIN;
    fds[1].fd = sock_fd;
    fds[1].events = POLLIN;

    // Token bucket
    double bytes_per_ms = INIT_BYTES_PER_MS;
    double tokens = 0.0;
    int token_scale_cnt = 0;

    struct timeval last_refill;
    struct timeval last_sent;
    gettimeofday(&last_refill, NULL);
    gettimeofday(&last_sent, NULL);

    int rand_delay_ms = 0;
    srand(time(NULL));

    while (1) {
        //memset(buffer, 0, BUF_SIZE);
        LOG(LOG_DEBUG, "Entering poll()");
        int ret = poll(fds, 2, -1);
        LOG(LOG_DEBUG, "poll() returned %d", ret);
        if (ret < 0) {
            perror("poll()");
            cleanup(0);
            exit(1);
        }
        
        // Check last sent
        struct timeval now;
        gettimeofday(&now, NULL);
        double elapsed_since_sent_ms = (now.tv_sec - last_sent.tv_sec) * 1000.0 +
                                       (now.tv_usec - last_sent.tv_usec) / 1000.0;
        if (elapsed_since_sent_ms >= 10.0) { // No traffic for 10ms: reset
            bytes_per_ms = INIT_BYTES_PER_MS;
            token_scale_cnt = 0;
            LOG(LOG_DEBUG, "Reset traffic control due to idle traffic state.");
        }
        // Refill tokens every 1ms
        double elapsed_refill_ms = (now.tv_sec - last_refill.tv_sec) * 1000.0 +
                                   (now.tv_usec - last_refill.tv_usec) / 1000.0;
        if (elapsed_refill_ms >= 1){
            if (tokens <= 0) token_scale_cnt += 1;
            tokens += elapsed_refill_ms * bytes_per_ms;
            if (tokens > bytes_per_ms){
                tokens = bytes_per_ms;
                token_scale_cnt -= 1;
            }
            last_refill = now;
        }
        // Adaptive scaling
        if (token_scale_cnt >= 10) {
            bytes_per_ms *= 1.5;
            if (bytes_per_ms > BUF_SIZE) // put a reasonable cap
                bytes_per_ms = BUF_SIZE;
            token_scale_cnt = 0;
        }
        else if(token_scale_cnt <= -10){
            bytes_per_ms = MAX(INIT_BYTES_PER_MS, bytes_per_ms*0.5);
            token_scale_cnt = 0;
        }

        //////////////////////////////////////////////////////////////////
        // === TUN -> SERVER (Keeping traffic in ctl_buf) === ////////////
        //////////////////////////////////////////////////////////////////
        if (fds[0].revents & POLLIN && tokens > 0 && elapsed_since_sent_ms >= rand_delay_ms) {
            int nread = read(tun_fd, buffer, BUF_SIZE);
            if (nread > 0) {
                uint16_t hdr = htons(nread);
                if (write(sock_fd, &hdr, 2) != 2){
                    perror("TUN->Proxy header write()");
                }
                if (write(sock_fd, buffer, nread) != nread){
                    perror("TUN->Proxy payload write()");
                }
                tokens -= nread + 2;
                rand_delay_ms = (double)rand() / RAND_MAX;
                gettimeofday(&last_sent, NULL);
            }
        }

        /////////////////////////////////////////////
        // === SERVER -> TUN (no merge needed) === //
        /////////////////////////////////////////////
        if (fds[1].revents & POLLIN) {
            LOG(LOG_TRACE, "sock_fd is readable");

            int n = read_framed_packet(sock_fd, buffer, BUF_SIZE);
            LOG(LOG_TRACE, "read_framed_packet(sock_fd) returned %d", n);

            if (n > 0) {
                int w = write(tun_fd, buffer, n);
                if (w < 0) {
                    LOG(LOG_ERROR, "write(tun_fd) failed: Tried writing %d bytes to tun_fd=%d", n, tun_fd);
                    perror("write(tun_fd)");
                    LOG(LOG_DEBUG, "First 16 bytes of buffer: ");
                    for (int i = 0; i < 16 && i < n; ++i) {
                        fprintf(stderr, "%02x ", (unsigned char)buffer[i]);
                    }
                    fprintf(stderr, "\n");
                } else {
                    LOG(LOG_DEBUG, " wrote %d bytes to tun_fd", w);
                }
            } else if (n == 0) {
                LOG(LOG_TRACE, "No complete framed packet available (n == 0)");
            } else {
                LOG(LOG_WARN, "Server closed connection or framing error");
                // Optionally handle disconnect here
            }
        }else if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG(LOG_ERROR, "sock_fd poll error: revents = 0x%x", fds[0].revents);
            break;  // or handle cleanup
        }
    }
    close(tun_fd);
    close(sock_fd);
    cleanup(0);
    return 0;
}
