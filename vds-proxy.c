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
    uint8_t     l2_bdaddr[6];
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

int connect_unix_pipe(const char *pipe_name, const uint8_t *mac_bytes) {
    // Anti-EINPROGRESS: Blockierende Erstellung zur Beseitigung von Multiplexer-Races
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    
    // Invariante: Erstes Byte muss zwingend '\0' sein (Abstrakter Namespace)
    addr.sun_path[0] = '\0';
    
    // KORREKTUR: Alle 3 Zeichen ("v_c" oder "v_i") komplett ab Index 1 kopieren
    memcpy(&addr.sun_path[1], pipe_name, 3);
    
    // Invariante: Kernel-Längenformel für das 4-Byte-Muster (1x Nullbyte + 3x String-Zeichen)
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 4;
    
    // Blockienter Verbindungsaufbau verhindert Multiplexer-Races
    if (connect(sock, (struct sockaddr *)&addr, len) < 0) {
        close(sock);
        return -1;
    }
    
    // INVARIANTE: MAC-Injektion via Stream-Payload direkt nach Aufbau
    if (send(sock, mac_bytes, 6, MSG_NOSIGNAL) != 6) {
        close(sock);
        return -1;
    }
    
    // Erst jetzt den Socket in den Zustand O_NONBLOCK versetzen
    if (set_nonblocking_fd(sock) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main(void) {
    // Senderschutz erzwingen
    signal(SIGPIPE, SIG_IGN);

    // Unbestechliche Zeilenpufferung erzwingen
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

        fds[IDX_CLI_CTRL].fd  = client_ctrl;  fds[IDX_CLI_CTRL].events  = (client_ctrl >= 0) ? POLLIN : 0;
        fds[IDX_VDSD_CTRL].fd = vdsd_ctrl;    fds[IDX_VDSD_CTRL].events = (vdsd_ctrl >= 0) ? POLLIN : 0;
        fds[IDX_CLI_INTR].fd  = client_intr;  fds[IDX_CLI_INTR].events  = (client_intr >= 0) ? POLLIN : 0;
        fds[IDX_VDSD_INTR].fd = vdsd_intr;    fds[IDX_VDSD_INTR].events = (vdsd_intr >= 0) ? POLLIN : 0;

        int ret = poll(fds, TOTAL_FDS, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // --- 1. ASYNCHRONES ABFANGEN & DIREKTKOPPLUNG ---
        if (fds[IDX_SRV_CTRL].fd >= 0 && (fds[IDX_SRV_CTRL].revents & POLLIN)) {
            struct custom_sockaddr_l2 saddr;
            socklen_t slen = sizeof(saddr);
            int tmp = accept4(srv_ctrl, (struct sockaddr *)&saddr, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (tmp >= 0) {
                client_ctrl = tmp;
                printf("vDS-Proxy: Controller Control-Kanal aktiv abgefangen.\n");
                
                vdsd_ctrl = connect_unix_pipe("v_c", saddr.l2_bdaddr);
                if (vdsd_ctrl >= 0) {
                    printf("vDS-Proxy: Control-Pipeline erfolgreich aktiv geschaltet.\n");
                } else {
                    fprintf(stderr, "vDS-Proxy: Fehler beim Verbinden mit @v_c\n");
                    close(client_ctrl); client_ctrl = -1;
                }
            }
        }

        if (fds[IDX_SRV_INTR].fd >= 0 && (fds[IDX_SRV_INTR].revents & POLLIN)) {
            struct custom_sockaddr_l2 saddr;
            socklen_t slen = sizeof(saddr);
            int tmp = accept4(srv_intr, (struct sockaddr *)&saddr, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (tmp >= 0) {
                client_intr = tmp;
                printf("vDS-Proxy: Controller Interrupt-Kanal aktiv abgefangen.\n");
                
                vdsd_intr = connect_unix_pipe("v_i", saddr.l2_bdaddr);
                if (vdsd_intr >= 0) {
                    printf("vDS-Proxy: Interrupt-Pipeline erfolgreich aktiv geschaltet.\n");
                } else {
                    fprintf(stderr, "vDS-Proxy: Fehler beim Verbinden mit @v_i\n");
                    close(client_intr); client_intr = -1;
                }
            }
        }

        // --- 2. CRITICAL HARDWARE ERROR-HANDLING ---
        if (client_ctrl >= 0 && (fds[IDX_CLI_CTRL].revents & (POLLERR | POLLNVAL))) goto shutdown_control;
        if (vdsd_ctrl >= 0   && (fds[IDX_VDSD_CTRL].revents & (POLLERR | POLLNVAL))) goto shutdown_control;
        if (client_intr >= 0 && (fds[IDX_CLI_INTR].revents & (POLLERR | POLLNVAL))) goto shutdown_interrupt;
        if (vdsd_intr >= 0   && (fds[IDX_VDSD_INTR].revents & (POLLERR | POLLNVAL))) goto shutdown_interrupt;

        // --- 3. PIPELINE ROUTING MIT SPEZIFIKATIONSKONFORMEM PEEK-SCHUTZ ---
        if (client_ctrl >= 0 && (fds[IDX_CLI_CTRL].revents & POLLIN)) {
            ssize_t len = recv(client_ctrl, heap_buffer, 1024, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (len > 0) {
                if (vdsd_ctrl >= 0) send(vdsd_ctrl, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            } else if (len == 0) {
                char test_ch;
                ssize_t check = recv(client_ctrl, &test_ch, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
                if (check == 0) goto shutdown_control;
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_control; 
            }
        }

        if (vdsd_ctrl >= 0 && (fds[IDX_VDSD_CTRL].revents & POLLIN)) {
            ssize_t len = recv(vdsd_ctrl, heap_buffer, 1024, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (len > 0) {
                if (client_ctrl >= 0) send(client_ctrl, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            } else if (len == 0) {
                char test_ch;
                ssize_t check = recv(vdsd_ctrl, &test_ch, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
                if (check == 0) goto shutdown_control;
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_control; 
            }
        }

        if (client_intr >= 0 && (fds[IDX_CLI_INTR].revents & POLLIN)) {
            ssize_t len = recv(client_intr, heap_buffer, 1024, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (len > 0) {
                if (vdsd_intr >= 0) send(vdsd_intr, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            } else if (len == 0) {
                char test_ch;
                ssize_t check = recv(client_intr, &test_ch, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
                if (check == 0) goto shutdown_interrupt;
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_interrupt;
            }
        }

        if (vdsd_intr >= 0 && (fds[IDX_VDSD_INTR].revents & POLLIN)) {
            ssize_t len = recv(vdsd_intr, heap_buffer, 1024, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (len > 0) {
                if (client_intr >= 0) send(client_intr, heap_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            } else if (len == 0) {
                char test_ch;
                ssize_t check = recv(vdsd_intr, &test_ch, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
                if (check == 0) goto shutdown_interrupt;
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                goto shutdown_interrupt;
            }
        }
        
        // --- 4. VERZÖGERTES POLLHUP-HANDLING ---
        if ((fds[IDX_CLI_CTRL].revents & POLLHUP) || (fds[IDX_VDSD_CTRL].revents & POLLHUP)) {
            if (!(fds[IDX_CLI_CTRL].revents & POLLIN) && !(fds[IDX_VDSD_CTRL].revents & POLLIN)) {
                goto shutdown_control;
            }
        }
        if ((fds[IDX_CLI_INTR].revents & POLLHUP) || (fds[IDX_VDSD_INTR].revents & POLLHUP)) {
            if (!(fds[IDX_CLI_INTR].revents & POLLIN) && !(fds[IDX_VDSD_INTR].revents & POLLIN)) {
                goto shutdown_interrupt;
            }
        }
        
        continue;

    shutdown_control:
        printf("vDS-Proxy: Control-Pipeline getrennt.\n");
        if (client_ctrl >= 0) close(client_ctrl);
        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        client_ctrl = -1; vdsd_ctrl = -1;
        continue;

    shutdown_interrupt:
        printf("vDS-Proxy: Interrupt-Pipeline getrennt.\n");
        if (client_intr >= 0) close(client_intr);
        if (vdsd_intr >= 0) close(vdsd_intr);
        client_intr = -1; vdsd_intr = -1;
        continue;
    }

    free(heap_buffer);
    close(srv_ctrl);
    close(srv_intr);
    return 0;
}
