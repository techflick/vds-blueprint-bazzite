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
#include <sched.h>
#include <signal.h>

#define BT_AF_BLUETOOTH   31
#define BT_SOCK_SEQPACKET 5
#define BT_BTPROTO_L2CAP  0

#define IDX_SRV_CTRL   0
#define IDX_SRV_INTR   1
#define IDX_CLI_CTRL   2
#define IDX_VDSD_CTRL  3
#define IDX_CLI_INTR   4
#define IDX_VDSD_INTR  5
#define TOTAL_FDS      6

struct custom_sockaddr_l2 {
    uint16_t    l2_family;
    uint16_t    l2_psm;
    uint8_t     l2_bdaddr;
    uint16_t    l2_cid;
    uint8_t     l2_bdaddr_type;
};

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
    addr.l2_psm = psm; 
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

int connect_unix_pipe(const char *name_three_bytes) {
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    
    // Strikte 6-Byte-Regel einhalten: Erstes Byte ist \0, dann 3 Bytes Name
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name_three_bytes, 3); 
    
    // offsetof + 1 (für \0) + 3 (Nutzdaten) = exakt 4 zusätzliche Bytes
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 4;
    
    if (connect(sock, (struct sockaddr *)&addr, len) < 0) {
        close(sock);
        return -1;
    }
    
    usleep(2000);
    
    if (set_nonblocking_fd(sock) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main(void) {
    // Signal-Absturzsicherung: SIGPIPE global ignorieren [2]
    signal(SIGPIPE, SIG_IGN);

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
        
        fds[IDX_SRV_CTRL].fd = (client_ctrl < 0) ? srv_ctrl : -1;
        fds[IDX_SRV_CTRL].events = POLLIN;
        
        fds[IDX_SRV_INTR].fd = (client_intr < 0) ? srv_intr : -1;
        fds[IDX_SRV_INTR].events = POLLIN;

        // Zustandsgesteuerte Selektivüberwachung gegen Kernel-Deadlocks [2]
        fds[IDX_CLI_CTRL].fd  = client_ctrl;  fds[IDX_CLI_CTRL].events  = (client_ctrl >= 0) ? POLLIN : 0;
        fds[IDX_VDSD_CTRL].fd = vdsd_ctrl;    fds[IDX_VDSD_CTRL].events = (vdsd_ctrl >= 0) ? POLLIN : 0;
        fds[IDX_CLI_INTR].fd  = client_intr;  fds[IDX_CLI_INTR].events  = (client_intr >= 0) ? POLLIN : 0;
        fds[IDX_VDSD_INTR].fd = vdsd_intr;    fds[IDX_VDSD_INTR].events = (vdsd_intr >= 0) ? POLLIN : 0;

        int ret = poll(fds, TOTAL_FDS, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[IDX_SRV_CTRL].fd >= 0 && (fds[IDX_SRV_CTRL].revents & POLLIN)) {
            int tmp = accept(srv_ctrl, NULL, NULL);
            if (tmp >= 0) {
                client_ctrl = tmp;
                fcntl(client_ctrl, F_SETFD, FD_CLOEXEC);
                set_nonblocking_fd(client_ctrl);
                printf("vDS-Proxy: Controller Control-Kanal aktiv abgefangen.\n");
            }
        }

        if (fds[IDX_SRV_INTR].fd >= 0 && (fds[IDX_SRV_INTR].revents & POLLIN)) {
            int tmp = accept(srv_intr, NULL, NULL);
            if (tmp >= 0) {
                client_intr = tmp;
                fcntl(client_intr, F_SETFD, FD_CLOEXEC);
                set_nonblocking_fd(client_intr);
                printf("vDS-Proxy: Controller Interrupt-Kanal aktiv abgefangen.\n");
            }
        }

        // Synchroner Doppel-Connect Brückenschlag zu vdsd [2]
        if (client_ctrl >= 0 && client_intr >= 0 && vdsd_ctrl < 0 && vdsd_intr < 0) {
            printf("vDS-Proxy: Beide Bluetooth-Kanaele gesichert. Verbinde RAM-Pipelines...\n");
            vdsd_ctrl = connect_unix_pipe("v_c");
            vdsd_intr = connect_unix_pipe("v_i");
            if (vdsd_ctrl >= 0 && vdsd_intr >= 0) {
                printf("vDS-Proxy: Beide Speicher-Pipelines erfolgreich instanziiert. Tunnel aktiv.\n");
                fflush(stderr); // Erzwinge Flush nach Setup laut Vorgabe [2]
                // KEIN continue hier! Wir lassen den Loop weiterlaufen, um die FDs direkt zu verarbeiten.
            } else {
                fprintf(stderr, "vDS-Proxy: FATAL - IPC-Verbindung zum vdsd fehlgeschlagen.\n");
                if (vdsd_ctrl >= 0) { close(vdsd_ctrl); vdsd_ctrl = -1; }
                if (vdsd_intr >= 0) { close(vdsd_intr); vdsd_intr = -1; }
                close(client_ctrl); client_ctrl = -1;
                close(client_intr); client_intr = -1;
                continue;
            }
        }

        // Striktes POLLHUP/Fehler-Handling [2] (Nur auswerten, wenn poll() auch wirklich Events für diesen FD gemeldet hat)
        if (client_ctrl >= 0 && fds[IDX_CLI_CTRL].fd >= 0) {
            if (fds[IDX_CLI_CTRL].revents & (POLLERR | POLLNVAL)) goto shutdown_control;
            if ((fds[IDX_CLI_CTRL].revents & POLLHUP) && !(fds[IDX_CLI_CTRL].revents & POLLIN)) goto shutdown_control;
        }

        if (client_intr >= 0 && fds[IDX_CLI_INTR].fd >= 0) {
            if (fds[IDX_CLI_INTR].revents & (POLLERR | POLLNVAL)) goto shutdown_interrupt;
            if ((fds[IDX_CLI_INTR].revents & POLLHUP) && !(fds[IDX_CLI_INTR].revents & POLLIN)) goto shutdown_interrupt;
        }
        
        if (vdsd_ctrl >= 0 && fds[IDX_VDSD_CTRL].fd >= 0) {
            if (fds[IDX_VDSD_CTRL].revents & (POLLERR | POLLNVAL)) goto shutdown_control;
            if ((fds[IDX_VDSD_CTRL].revents & POLLHUP) && !(fds[IDX_VDSD_CTRL].revents & POLLIN)) goto shutdown_control;
        }

        if (vdsd_intr >= 0 && fds[IDX_VDSD_INTR].fd >= 0) {
            if (fds[IDX_VDSD_INTR].revents & (POLLERR | POLLNVAL)) goto shutdown_interrupt;
            if ((fds[IDX_VDSD_INTR].revents & POLLHUP) && !(fds[IDX_VDSD_INTR].revents & POLLIN)) goto shutdown_interrupt;
        }

        // --- CONTROL KANAL DATA ROUTING ---
        if (client_ctrl >= 0 && (fds[IDX_CLI_CTRL].revents & POLLIN)) {
            ssize_t len = recv(client_ctrl, heap_buffer, 1024, 0);
            if (len > 0) {
                if (vdsd_ctrl >= 0) {
                    send(vdsd_ctrl, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
                }
            } else if (len == 0) {
                // Bei asynchronen L2CAP-Sockets kann ein len == 0 im Handshake ein valides Signal sein, 
                // sofern kein Fehler vorliegt und EAGAIN nicht geworfen wurde. Wir sichern das ab:
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    goto shutdown_control; 
                }
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_control;
            }
        }

        if (vdsd_ctrl >= 0 && (fds[IDX_VDSD_CTRL].revents & POLLIN)) {
            ssize_t len = recv(vdsd_ctrl, heap_buffer, 1024, 0);
            if (len > 0) {
                send(client_ctrl, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            } else if (len == 0) {
                goto shutdown_control; // Zero-Length RAM-Kanal = Tunnel-Ende [2]
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_control;
            }
        }

        // --- INTERRUPT KANAL DATA ROUTING ---
        if (client_intr >= 0 && (fds[IDX_CLI_INTR].revents & POLLIN)) {
            ssize_t len = recv(client_intr, heap_buffer, 1024, 0);
            if (len > 0) {
                if (vdsd_intr >= 0) {
                    send(vdsd_intr, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
                }
            } else if (len == 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    goto shutdown_interrupt; 
                }
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_interrupt;
            }
        }

        if (vdsd_intr >= 0 && (fds[IDX_VDSD_INTR].revents & POLLIN)) {
            ssize_t len = recv(vdsd_intr, heap_buffer, 1024, 0);
            if (len > 0) {
                send(client_intr, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            } else if (len == 0) {
                goto shutdown_interrupt; // Zero-Length RAM-Kanal = Tunnel-Ende [2]
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_interrupt;
            }
        }
        continue;

    shutdown_control:
        printf("vDS-Proxy: Control-Pipeline getrennt (System-Errno: %d - %s).\n", errno, strerror(errno));
        if (client_ctrl >= 0) close(client_ctrl);
        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        client_ctrl = -1; vdsd_ctrl = -1;
        continue;

    shutdown_interrupt:
        printf("vDS-Proxy: Interrupt-Pipeline getrennt (System-Errno: %d - %s).\n", errno, strerror(errno));
        if (client_intr >= 0) close(client_intr);
        if (vdsd_intr >= 0) close(vdsd_intr);
        client_intr = -1; vdsd_intr = -1;
        continue;
    }
