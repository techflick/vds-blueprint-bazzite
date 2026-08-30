#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <errno.h>

#define BT_AF_BLUETOOTH   31
#define BT_SOCK_SEQPACKET 5
#define BT_BTPROTO_L2CAP  0

struct srv_poll_layout {
    struct pollfd c_fd;
    struct pollfd i_fd;
};

struct shuttle_poll_layout {
    struct pollfd cc;
    struct pollfd vc;
    struct pollfd ci;
    struct pollfd vi;
};

int set_nonblocking_fd(int fd) {
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
    
    // Dynamisches Server-Binding ueber die Wildcard (00:00:00:00:00:00)
    // Dadurch lauscht der Proxy auf JEDEM verbauten Bluetooth-Dongle universell!
    uint8_t addr_bytes[16];
    memset(addr_bytes, 0, 16);
    addr_bytes[0] = BT_AF_BLUETOOTH & 0xFF;
    addr_bytes[1] = (BT_AF_BLUETOOTH >> 8) & 0xFF;
    addr_bytes[2] = psm & 0xFF;
    addr_bytes[3] = (psm >> 8) & 0xFF;

    if (bind(sock, (struct sockaddr *)addr_bytes, 16) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, 5) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int connect_unix_pipe(const char *path) {
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock < 0) return -1;
    if (set_nonblocking_fd(sock) < 0) {
        close(sock);
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    
    int res = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    printf("vDS-Proxy: Starte klammerfreie UNIX-IPC Routing-Infrastruktur...\n");

    int srv_ctrl = open_bt_server_link(0x11);
    int srv_intr = open_bt_server_link(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) {
        return 1;
    }

    int client_ctrl = -1, client_intr = -1;
    int vdsd_ctrl = -1, vdsd_intr = -1;
    int state = 0;

    void *heap_buffer = malloc(1024);
    if (!heap_buffer) return 1;

    printf("vDS-Proxy: Initialisierung erfolgreich. Warte auf DualSense-Controller...\n");

    while (1) {
        if (state == 0) {
            struct srv_poll_layout srv_fds;
            memset(&srv_fds, 0, sizeof(srv_fds));
            srv_fds.c_fd.fd = srv_ctrl; srv_fds.c_fd.events = POLLIN;
            srv_fds.i_fd.fd = srv_intr; srv_fds.i_fd.events = POLLIN;

            int ret_c = poll(&srv_fds.c_fd, 1, 100);
            int ret_i = poll(&srv_fds.i_fd, 1, 100);

            if (ret_c > 0 && (srv_fds.c_fd.revents & POLLIN)) {
                struct sockaddr_storage remote;
                socklen_t len = sizeof(remote);
                int tmp = accept(srv_ctrl, (struct sockaddr *)&remote, &len);
                if (tmp >= 0) {
                    if (client_ctrl >= 0) close(client_ctrl);
                    client_ctrl = tmp;
                    set_nonblocking_fd(client_ctrl);
                    printf("vDS-Proxy: Controller Control-Kanal aktiv abgefangen.\n");
                    uint8_t peek = 0;
                    recv(client_ctrl, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
                }
            }

            if (ret_i > 0 && (srv_fds.i_fd.revents & POLLIN)) {
                struct sockaddr_storage remote;
                socklen_t len = sizeof(remote);
                int tmp = accept(srv_intr, (struct sockaddr *)&remote, &len);
                if (tmp >= 0) {
                    if (client_intr >= 0) close(client_intr);
                    client_intr = tmp;
                    set_nonblocking_fd(client_intr);
                    printf("vDS-Proxy: Controller Interrupt-Kanal aktiv abgefangen.\n");
                }
            }

            if (client_ctrl >= 0 && client_intr >= 0) {
                printf("vDS-Proxy: Reiche Daten ueber RAM-Sockets an den Daemon weiter...\n");
                
                // Absolute Hardware-Unabhaengigkeit: Wir verbinden uns rein ueber lokale Sockets im Dateisystem!
                vdsd_ctrl = connect_unix_pipe("v_c");
                vdsd_intr = connect_unix_pipe("v_i");

                if (vdsd_ctrl >= 0 && vdsd_intr >= 0) {
                    printf("vDS-Proxy: **Latenzfreie Speicher-Pipeline aktiv!**\n");
                    state = 1;
                    continue;
                }
                goto shutdown_link;
            }
        } else {
            struct shuttle_poll_layout tunnel_fds;
            memset(&tunnel_fds, 0, sizeof(tunnel_fds));
            tunnel_fds.cc.fd = client_ctrl; tunnel_fds.cc.events = POLLIN;
            tunnel_fds.vc.fd = vdsd_ctrl;   tunnel_fds.vc.events = POLLIN;
            tunnel_fds.ci.fd = client_intr; tunnel_fds.ci.events = POLLIN;
            tunnel_fds.vi.fd = vdsd_intr;   tunnel_fds.vi.events = POLLIN;

            poll(&tunnel_fds.cc, 1, 5); poll(&tunnel_fds.vc, 1, 5);
            poll(&tunnel_fds.ci, 1, 5); poll(&tunnel_fds.vi, 1, 5);

            if ((tunnel_fds.cc.revents | tunnel_fds.vc.revents | tunnel_fds.ci.revents | tunnel_fds.vi.revents) & (POLLHUP | POLLERR | POLLNVAL)) {
                goto shutdown_link;
            }

            if (tunnel_fds.cc.revents & POLLIN) {
                int len = recv(client_ctrl, heap_buffer, 1024, 0);
                if (len <= 0) goto shutdown_link;
                send(vdsd_ctrl, heap_buffer, len, 0);
            }
            if (tunnel_fds.vc.revents & POLLIN) {
                int len = recv(vdsd_ctrl, heap_buffer, 1024, 0);
                if (len <= 0) goto shutdown_link;
                send(client_ctrl, heap_buffer, len, 0);
            }
            if (tunnel_fds.ci.revents & POLLIN) {
                int len = recv(client_intr, heap_buffer, 1024, 0);
                if (len <= 0) goto shutdown_link;
                send(vdsd_intr, heap_buffer, len, 0);
            }
            if (tunnel_fds.vi.revents & POLLIN) {
                int len = recv(vdsd_intr, heap_buffer, 1024, 0);
                if (len <= 0) goto shutdown_link;
                send(client_intr, heap_buffer, len, 0);
            }
            continue;

        shutdown_link:
            printf("vDS-Proxy: Pipeline-Verbindung getrennt. Setze Routing-Infrastruktur zurueck...\n");
            if (client_ctrl >= 0) close(client_ctrl);
            if (client_intr >= 0) close(client_intr);
            if (vdsd_ctrl >= 0) close(vdsd_ctrl);
            if (vdsd_intr >= 0) close(vdsd_intr);
            client_ctrl = client_intr = vdsd_ctrl = vdsd_intr = -1;
            state = 0;
        }
    }
    free(heap_buffer);
    return 0;
}
