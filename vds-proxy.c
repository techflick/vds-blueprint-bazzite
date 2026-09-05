#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <errno.h>

#define BT_AF_BLUETOOTH   31
#define BT_SOCK_SEQPACKET 5
#define BT_BTPROTO_L2CAP  0

// Erweiterte Indizes für eine flache, unblockierte Hauptschleife
#define IDX_SRV_CTRL   0
#define IDX_SRV_INTR   1
#define IDX_CLI_CTRL   2
#define IDX_VDSD_CTRL  3
#define IDX_CLI_INTR   4
#define IDX_VDSD_INTR  5
#define TOTAL_FDS      6

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

int connect_unix_pipe(const char *name_three_bytes) {
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    memcpy(&addr.sun_path[1], name_three_bytes, 3); 
    
    socklen_t len = sizeof(struct sockaddr_un);
    if (connect(sock, (struct sockaddr *)&addr, len) < 0) {
        close(sock);
        return -1;
    }
    if (set_nonblocking_fd(sock) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    printf("vDS-Proxy: Starte klammerfreie UNIX-IPC Routing-Infrastruktur...\n");

    int srv_ctrl = open_bt_server_link(0x11);
    int srv_intr = open_bt_server_link(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) {
        fprintf(stderr, "vDS-Proxy: Fehler beim Erstellen der Bluetooth-Serverlinks.\n");
        return 1;
    }

    int client_ctrl = -1, vdsd_ctrl = -1;
    int client_intr = -1, vdsd_intr = -1;

    void *heap_buffer = malloc(1024);
    if (!heap_buffer) return 1;

    printf("vDS-Proxy: Initialisierung erfolgreich. Warte auf DualSense-Controller...\n");

    struct pollfd fds[TOTAL_FDS];

    while (1) {
        memset(fds, 0, sizeof(fds));
        
        // Server immer abhören, wenn kein Client verbunden ist
        fds[IDX_SRV_CTRL].fd = (client_ctrl < 0) ? srv_ctrl : -1;
        fds[IDX_SRV_CTRL].events = POLLIN;
        
        fds[IDX_SRV_INTR].fd = (client_intr < 0) ? srv_intr : -1;
        fds[IDX_SRV_INTR].events = POLLIN;

        // Aktive Datenkanäle dynamisch in den Poll-Array hängen
        fds[IDX_CLI_CTRL].fd  = client_ctrl;  fds[IDX_CLI_CTRL].events  = (client_ctrl >= 0) ? POLLIN : 0;
        fds[IDX_VDSD_CTRL].fd = vdsd_ctrl;    fds[IDX_VDSD_CTRL].events = (vdsd_ctrl >= 0) ? POLLIN : 0;
        fds[IDX_CLI_INTR].fd  = client_intr;  fds[IDX_CLI_INTR].events  = (client_intr >= 0) ? POLLIN : 0;
        fds[IDX_VDSD_INTR].fd = vdsd_intr;    fds[IDX_VDSD_INTR].events = (vdsd_intr >= 0) ? POLLIN : 0;

        int ret = poll(fds, TOTAL_FDS, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // --- ASYNCHRONER ABFANG-MECHANISMUS ---
        if (fds[IDX_SRV_CTRL].revents & POLLIN) {
            int tmp = accept(srv_ctrl, NULL, NULL);
            if (tmp >= 0) {
                client_ctrl = tmp;
                set_nonblocking_fd(client_ctrl);
                printf("vDS-Proxy: Controller Control-Kanal aktiv abgefangen.\n");
                vdsd_ctrl = connect_unix_pipe("v_c");
                if (vdsd_ctrl >= 0) {
                    printf("vDS-Proxy: Control-Speicher-Pipeline instanziiert.\n");
                }
            }
        }

        if (fds[IDX_SRV_INTR].revents & POLLIN) {
            int tmp = accept(srv_intr, NULL, NULL);
            if (tmp >= 0) {
                client_intr = tmp;
                set_nonblocking_fd(client_intr);
                printf("vDS-Proxy: Controller Interrupt-Kanal aktiv abgefangen.\n");
                vdsd_intr = connect_unix_pipe("v_i");
                if (vdsd_intr >= 0) {
                    printf("vDS-Proxy: Interrupt-Speicher-Pipeline instanziiert.\n");
                }
            }
        }

        // --- STRIKTE FEHLERPRÜFUNG (Klammerfrei via fixierte Indizes) ---
        if (client_ctrl >= 0 && (fds[IDX_CLI_CTRL].revents & (POLLHUP | POLLERR | POLLNVAL))) goto shutdown_control;
        if (vdsd_ctrl >= 0   && (fds[IDX_VDSD_CTRL].revents & (POLLHUP | POLLERR | POLLNVAL))) goto shutdown_control;
        if (client_intr >= 0 && (fds[IDX_CLI_INTR].revents & (POLLHUP | POLLERR | POLLNVAL))) goto shutdown_interrupt;
        if (vdsd_intr >= 0   && (fds[IDX_VDSD_INTR].revents & (POLLHUP | POLLERR | POLLNVAL))) goto shutdown_interrupt;

        // --- DATA STREAMING: CONTROL KANAL (Autonom & Unblockiert) ---
        if (client_ctrl >= 0 && vdsd_ctrl >= 0) {
            if (fds[IDX_CLI_CTRL].revents & POLLIN) {
                ssize_t len = recv(client_ctrl, heap_buffer, 1024, 0);
                if (len > 0) send(vdsd_ctrl, heap_buffer, len, 0);
                else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_control;
                else if (len == 0) goto shutdown_control;
            }
            if (fds[IDX_VDSD_CTRL].revents & POLLIN) {
                ssize_t len = recv(vdsd_ctrl, heap_buffer, 1024, 0);
                if (len > 0) send(client_ctrl, heap_buffer, len, 0);
                else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_control;
                else if (len == 0) goto shutdown_control;
            }
        }

        // --- DATA STREAMING: INTERRUPT KANAL (Autonom & Unblockiert) ---
        if (client_intr >= 0 && vdsd_intr >= 0) {
            if (fds[IDX_CLI_INTR].revents & POLLIN) {
                ssize_t len = recv(client_intr, heap_buffer, 1024, 0);
                if (len > 0) send(vdsd_intr, heap_buffer, len, 0);
                else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_interrupt;
                else if (len == 0) goto shutdown_interrupt;
            }
            if (fds[IDX_VDSD_INTR].revents & POLLIN) {
                ssize_t len = recv(vdsd_intr, heap_buffer, 1024, 0);
                if (len > 0) send(client_intr, heap_buffer, len, 0);
                else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_interrupt;
                else if (len == 0) goto shutdown_interrupt;
            }
        }
        continue;

    shutdown_control:
        printf("vDS-Proxy: Control-Pipeline getrennt. Setze Kanal zurueck...\n");
        if (client_ctrl >= 0) close(client_ctrl);
        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        client_ctrl = -1; vdsd_ctrl = -1;
        continue;

    shutdown_interrupt:
        printf("vDS-Proxy: Interrupt-Pipeline getrennt. Setze Kanal zurueck...\n");
        if (client_intr >= 0) close(client_intr);
        if (vdsd_intr >= 0) close(vdsd_intr);
        client_intr = -1; vdsd_intr = -1;
        continue;
    }

    free(heap_buffer);
    close(srv_ctrl); close(srv_intr);
    return 0;
}
