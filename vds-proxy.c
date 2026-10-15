#include <sys/types.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>

#define BT_AF_BLUETOOTH   31
#define BT_SOCK_SEQPACKET 5
#define BT_BTPROTO_L2CAP  0

#define IDX_CLI_CTRL   0
#define IDX_VDSD_CTRL  1
#define IDX_CLI_INTR   2
#define IDX_VDSD_INTR  3
#define TOTAL_FDS      4

struct custom_sockaddr_l2 {
    uint16_t    l2_family;
    uint16_t    l2_psm;       
    uint8_t     l2_bdaddr[6];
    uint16_t    l2_cid;
    uint8_t     l2_bdaddr_type;
};

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HTOBS(n) (uint16_t)((((uint16_t)(n) & 0xff00) >> 8) | (((uint16_t)(n) & 0x00ff) << 8))
#else
#define HTOBS(n) (uint16_t)(n)
#endif

int set_nonblocking_fd(int fd) {
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int open_bt_server_link(uint16_t psm) {
    int sock = socket(BT_AF_BLUETOOTH, BT_SOCK_SEQPACKET, BT_BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (set_nonblocking_fd(sock) < 0) {
        close(sock);
        return -1;
    }
    
    struct custom_sockaddr_l2 addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = BT_AF_BLUETOOTH;
    addr.l2_psm = HTOBS(psm); 
    addr.l2_bdaddr_type = 0;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, 5) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int connect_unix_pipe(const char *pipe_name, const uint8_t *mac_bytes, int send_mac) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0); 
    if (sock < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0'; 
    
    size_t name_len = strlen(pipe_name);
    memcpy(&addr.sun_path[1], pipe_name, name_len);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + name_len;
    
    if (connect(sock, (struct sockaddr *)&addr, len) < 0) {
        close(sock);
        return -1;
    }
    if (send_mac && send(sock, mac_bytes, 6, MSG_NOSIGNAL) != 6) {
        close(sock);
        return -1;
    }
    if (set_nonblocking_fd(sock) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

ssize_t recv_stream_packet(int stream_fd, uint8_t *out_buf, size_t max_len) {
    uint16_t len_be;
    ssize_t n = recv(stream_fd, &len_be, 2, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 2) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
        if (n == 0) return -2; // Invariante: 0 im asynchronen Puffer ist KEIN Abbruch
        return -1; 
    }

    uint16_t packet_len = ntohs(len_be);
    if (packet_len > max_len) return -1; 

    n = recv(stream_fd, out_buf, 2 + packet_len, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < (2 + packet_len)) return -2;

    recv(stream_fd, out_buf, 2 + packet_len, MSG_DONTWAIT | MSG_NOSIGNAL);
    memmove(out_buf, out_buf + 2, packet_len);
    return packet_len;
}

ssize_t send_stream_packet(int stream_fd, const uint8_t *payload, size_t len) {
    uint16_t len_be = htons((uint16_t)len);
    if (send(stream_fd, &len_be, 2, MSG_DONTWAIT | MSG_NOSIGNAL) != 2) return -1;
    return send(stream_fd, payload, len, MSG_DONTWAIT | MSG_NOSIGNAL);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    int srv_ctrl = open_bt_server_link(0x11);
    int srv_intr = open_bt_server_link(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) return 1;

    int client_ctrl = -1, vdsd_ctrl = -1;
    int client_intr = -1, vdsd_intr = -1;
    uint8_t controller_mac[6] = {0};
    uint8_t *heap_buffer = (uint8_t *)malloc(1024);
    if (!heap_buffer) return 1;

    struct pollfd fds[TOTAL_FDS];
    int ret = 0, tmp = 0;
    ssize_t len = 0;

    while (1) {
        if (client_ctrl == -1 || client_intr == -1 || vdsd_ctrl == -1 || vdsd_intr == -1) {
            if (client_ctrl >= 0) close(client_ctrl);
            if (client_intr >= 0) close(client_intr);
            if (vdsd_ctrl >= 0)   close(vdsd_ctrl);
            if (vdsd_intr >= 0)   close(vdsd_intr);

            struct custom_sockaddr_l2 saddr;
            socklen_t slen = sizeof(saddr);
            struct pollfd poll_ctrl = { srv_ctrl, POLLIN, 0 };

            printf("vDS-Proxy: [STAGE 1] Warte auf Control-Kanal...\n");
            while (client_ctrl == -1) {
                if (poll(&poll_ctrl, 1, -1) > 0 && (poll_ctrl.revents & POLLIN)) {
                    tmp = accept4(srv_ctrl, (struct sockaddr *)&saddr, &slen, SOCK_CLOEXEC);
                    if (tmp >= 0) {
                        client_ctrl = tmp;
                        set_nonblocking_fd(client_ctrl);
                        memcpy(controller_mac, saddr.l2_bdaddr, 6);
                    }
                }
            }

            struct pollfd poll_intr = { srv_intr, POLLIN, 0 };
            printf("vDS-Proxy: [STAGE 2] Erwarten Interrupt-Kanal...\n");
            while (client_intr == -1) {
                if (poll(&poll_intr, 1, 2000) > 0 && (poll_intr.revents & POLLIN)) {
                    tmp = accept4(srv_intr, (struct sockaddr *)&saddr, &slen, SOCK_CLOEXEC);
                    if (tmp >= 0) {
                        client_intr = tmp;
                        set_nonblocking_fd(client_intr);
                    }
                } else { goto shutdown_global; }
            }

            vdsd_ctrl = connect_unix_pipe("v_c", controller_mac, 1);
            if (vdsd_ctrl < 0) goto shutdown_global;
            
            // Erhöhtes Intervall für das atomare Kaskaden-Mandat des Daemons
            usleep(15000); 
            vdsd_intr = connect_unix_pipe("v_i", controller_mac, 0);
            if (vdsd_intr < 0) goto shutdown_global;
            printf("vDS-Proxy: [IPC] Tunnel aktiv.\n");
        }

        for (int i = 0; i < TOTAL_FDS; i++) { fds[i].fd = -1; fds[i].events = 0; fds[i].revents = 0; }
        fds[IDX_CLI_CTRL].fd  = client_ctrl; fds[IDX_CLI_CTRL].events  = POLLIN;
        fds[IDX_VDSD_CTRL].fd = vdsd_ctrl;   fds[IDX_VDSD_CTRL].events = POLLIN;
        fds[IDX_CLI_INTR].fd  = client_intr; fds[IDX_CLI_INTR].events  = POLLIN;
        fds[IDX_VDSD_INTR].fd = vdsd_intr;   fds[IDX_VDSD_INTR].events = POLLIN;

        ret = poll(fds, TOTAL_FDS, -1);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        if (fds[IDX_CLI_CTRL].fd >= 0 && (fds[IDX_CLI_CTRL].revents & (POLLERR|POLLNVAL))) goto shutdown_global;
        if (fds[IDX_VDSD_CTRL].fd >= 0 && (fds[IDX_VDSD_CTRL].revents & (POLLERR|POLLNVAL))) goto shutdown_global;
        if (fds[IDX_CLI_INTR].fd >= 0 && (fds[IDX_CLI_INTR].revents & (POLLERR|POLLNVAL))) goto shutdown_global;
        if (fds[IDX_VDSD_INTR].fd >= 0 && (fds[IDX_VDSD_INTR].revents & (POLLERR|POLLNVAL))) goto shutdown_global;

        // I/O für Client Control
        if (fds[IDX_CLI_CTRL].fd >= 0 && (fds[IDX_CLI_CTRL].revents & POLLIN)) {
            len = recv(client_ctrl, heap_buffer, 1024, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (len > 0) {
                send_stream_packet(vdsd_ctrl, heap_buffer, len);
            } else if (len == 0) {
                // Invariante: Leerer asynchroner Puffer darf nicht trennen
                continue;
            } else if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                goto shutdown_global;
            }
        }

        // I/O für Daemon Control
        if (fds[IDX_VDSD_CTRL].fd >= 0 && (fds[IDX_VDSD_CTRL].revents & POLLIN)) {
            len = recv_stream_packet(vdsd_ctrl, heap_buffer, 1024);
            if (len > 0) send(client_ctrl, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            else if (len == -1) goto shutdown_global; 
        }

        // I/O für Client Interrupt
        if (fds[IDX_CLI_INTR].fd >= 0 && (fds[IDX_CLI_INTR].revents & POLLIN)) {
            len = recv(client_intr, heap_buffer, 1024, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (len > 0) {
                send_stream_packet(vdsd_intr, heap_buffer, len);
            } else if (len == 0) {
                continue;
            } else if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                goto shutdown_global;
            }
        }

        // I/O für Daemon Interrupt
        if (fds[IDX_VDSD_INTR].fd >= 0 && (fds[IDX_VDSD_INTR].revents & POLLIN)) {
            len = recv_stream_packet(vdsd_intr, heap_buffer, 1024);
            if (len > 0) send(client_intr, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            else if (len == -1) goto shutdown_global;
        }

        // HUP-Validierung erst nach der I/O-Filterung prüfen, um Fehlalarme zu blockieren
        if ((fds[IDX_CLI_CTRL].fd >= 0 && (fds[IDX_CLI_CTRL].revents & POLLHUP)) ||
            (fds[IDX_CLI_INTR].fd >= 0 && (fds[IDX_CLI_INTR].revents & POLLHUP))) {
            goto shutdown_global;
        }
        
        // Pipes dürfen erst nach erfolgreichem I/O-Durchlauf auf echten Verbindungsabriss geprüft werden
        if ((fds[IDX_VDSD_CTRL].fd >= 0 && (fds[IDX_VDSD_CTRL].revents & POLLHUP)) ||
            (fds[IDX_VDSD_INTR].fd >= 0 && (fds[IDX_VDSD_INTR].revents & POLLHUP))) {
            // Nur trennen, wenn davor kein auslesbarer Puffer den Zustand gerettet hat
            if (len == -1) goto shutdown_global;
        }
        continue;

    shutdown_global:
        printf("vDS-Proxy: [RESET] Globaler Kanal-Reset durchgeführt.\n");
        if (client_ctrl >= 0) close(client_ctrl); if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        if (client_intr >= 0) close(client_intr); if (vdsd_intr >= 0) close(vdsd_intr);
        client_ctrl = -1; vdsd_ctrl = -1; client_intr = -1; vdsd_intr = -1;
        memset(controller_mac, 0, 6); continue;
    }
    free(heap_buffer); close(srv_ctrl); close(srv_intr); return 0;
}
