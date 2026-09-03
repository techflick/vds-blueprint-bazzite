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

// Definiere feste Indizes für die Tunnel-Struktur (Verhindert Memory Corruption nach No-Go #1)
#define INDEX_CTRL_IN   0
#define INDEX_DAEMON_C  1
#define INDEX_INTR_IN   2
#define INDEX_DAEMON_I  3

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
    
    // 16-Byte-Erzwingung im echten BT-Kontext (No-Go #7)
    uint8_t addr_bytes [ 16 ];
    memset(addr_bytes, 0, 16);
    addr_bytes [ 0 ] = BT_AF_BLUETOOTH & 0xFF;
    addr_bytes [ 1 ] = (BT_AF_BLUETOOTH >> 8) & 0xFF;
    addr_bytes [ 2 ] = psm & 0xFF;
    addr_bytes [ 3 ] = (psm >> 8) & 0xFF;

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
    // SOCK_CLOEXEC schützt die FDs vor Leaks bei udev-Aufrufen
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    
    unsigned char raw_addr [ 14 ];
    memset(raw_addr, 0, 14);
    
    // AF_UNIX Familie (16-Bit Little Endian)
    raw_addr [ 0 ] = 1;
    raw_addr [ 1 ] = 0;
    
    // Abstraktes Socket-Layout: Erstes Byte von sun_path (Index 2) MUSS \0 sein
    raw_addr [ 2 ] = '\0'; 
    
    // Synchronisierter 1-Byte-Shift: Kopiert "v_c" oder "v_i" exakt an Index 3, 4, 5
    memcpy(&raw_addr [ 3 ], name_three_bytes, 3);
    
    int len = 14;

    if (connect(sock, (struct sockaddr *)raw_addr, len) < 0) {
        close(sock);
        return -1;
    }
    
    if (set_nonblocking_fd(sock) < 0) {
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
            struct pollfd srv_fds [ 2 ];
            memset(srv_fds, 0, sizeof(srv_fds));
            srv_fds [ 0 ].fd = srv_ctrl; srv_fds [ 0 ].events = POLLIN;
            srv_fds [ 1 ].fd = srv_intr; srv_fds [ 1 ].events = POLLIN;

            int ret = poll(srv_fds, 2, 100);
            if (ret > 0) {
                // ENTKOPPELTER CONTROL-KANAL HANDSHAKE
                if (srv_fds [ 0 ].revents & POLLIN) {
                    struct sockaddr_storage remote;
                    socklen_t len = sizeof(remote);
                    int tmp = accept(srv_ctrl, (struct sockaddr *)&remote, &len);
                    if (tmp >= 0) {
                        if (client_ctrl >= 0) close(client_ctrl);
                        client_ctrl = tmp;
                        set_nonblocking_fd(client_ctrl);
                        printf("vDS-Proxy: Controller Control-Kanal aktiv abgefangen.\n");
                        
                        // Sofort an den Daemon binden, um Timeouts zu verhindern
                        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
                        vdsd_ctrl = connect_unix_pipe("v_c");
                    }
                }
                // ENTKOPPELTER INTERRUPT-KANAL HANDSHAKE
                if (srv_fds [ 1 ].revents & POLLIN) {
                    struct sockaddr_storage remote;
                    socklen_t len = sizeof(remote);
                    int tmp = accept(srv_intr, (struct sockaddr *)&remote, &len);
                    if (tmp >= 0) {
                        if (client_intr >= 0) close(client_intr);
                        client_intr = tmp;
                        set_nonblocking_fd(client_intr);
                        printf("vDS-Proxy: Controller Interrupt-Kanal aktiv abgefangen.\n");
                        
                        // Sofort an den Daemon binden
                        if (vdsd_intr >= 0) close(vdsd_intr);
                        vdsd_intr = connect_unix_pipe("v_i");
                    }
                }
            }

            // Erst wenn alle 4 Endpunkte sauber im Speicher stehen, wird die Pipeline geschaltet
            if (client_ctrl >= 0 && client_intr >= 0 && vdsd_ctrl >= 0 && vdsd_intr >= 0) {
                printf("vDS-Proxy: **Latenzfreie Speicher-Pipeline aktiv!**\n");
                state = 1;
                continue;
            }
        } else {
            struct pollfd tunnel_fds [ 4 ];
            memset(tunnel_fds, 0, sizeof(tunnel_fds));
            tunnel_fds [ INDEX_CTRL_IN ].fd = client_ctrl; tunnel_fds [ INDEX_CTRL_IN ].events = POLLIN;
            tunnel_fds [ INDEX_DAEMON_C ].fd = vdsd_ctrl;   tunnel_fds [ INDEX_DAEMON_C ].events = POLLIN;
            tunnel_fds [ INDEX_INTR_IN ].fd = client_intr; tunnel_fds [ INDEX_INTR_IN ].events = POLLIN;
            tunnel_fds [ INDEX_DAEMON_I ].fd = vdsd_intr;   tunnel_fds [ INDEX_DAEMON_I ].events = POLLIN;

            int ret = poll(tunnel_fds, 4, 10);

            if (ret > 0) {
                // NO-GO FIX #5 & ERWEITERTES DEBUGGING
                for (int i = 0; i < 4; i++) {
                    if (tunnel_fds [ i ].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                        printf("vDS-Proxy: [DEBUG DROP] Abbruch durch Kernel-Signal (revents: %d) auf Tunnel-Index %d!\n", 
                               tunnel_fds [ i ].revents, i);
                        goto shutdown_link;
                    }
                }

                // INDEX 0: Controller Control -> Daemon Control
                if (tunnel_fds [ INDEX_CTRL_IN ].revents & POLLIN) {
                    int len = recv(client_ctrl, heap_buffer, 1024, 0);
                    if (len < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    } else if (len == 0) {
                        printf("vDS-Proxy: Controller hat Control-Kanal geschlossen.\n");
                        goto shutdown_link;
                    } else {
                        printf("vDS-Proxy: [CTRL IN] %d Bytes -> Daemon: ", len);
                        for(int i = 0; i < (len > 16 ? 16 : len); i++) {
                            printf("%02X ", ((unsigned char*)heap_buffer) [ i ]);
                        }
                        if (len > 16) printf("...");
                        printf("\n");

                        send(vdsd_ctrl, heap_buffer, len, 0);
                    }
                }

                // INDEX 1: Daemon Control -> Controller Control
                if (tunnel_fds [ INDEX_DAEMON_C ].revents & POLLIN) {
                    int len = recv(vdsd_ctrl, heap_buffer, 1024, 0);
                    if (len < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    } else if (len == 0) {
                        printf("vDS-Proxy: Daemon hat Control-Kanal geschlossen.\n");
                        goto shutdown_link;
                    } else {
                        printf("vDS-Proxy: [CTRL OUT] %d Bytes -> Controller\n", len);
                        send(client_ctrl, heap_buffer, len, 0);
                    }
                }
                
                // INDEX 2: Controller Interrupt -> Daemon Interrupt
                if (tunnel_fds [ INDEX_INTR_IN ].revents & POLLIN) {
                    int len = recv(client_intr, heap_buffer, 1024, 0);
                    if (len < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    } else if (len == 0) {
                        printf("vDS-Proxy: Controller hat Interrupt-Kanal geschlossen.\n");
                        goto shutdown_link;
                    } else {
                        printf("vDS-Proxy: [INTR IN] %d Bytes -> Daemon\n", len);
                        send(vdsd_intr, heap_buffer, len, 0);
                    }
                }
                
                // INDEX 3: Daemon Interrupt -> Controller Interrupt
                if (tunnel_fds [ INDEX_DAEMON_I ].revents & POLLIN) {
                    int len = recv(vdsd_intr, heap_buffer, 1024, 0);
                    if (len < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    } else if (len == 0) {
                        printf("vDS-Proxy: Daemon hat Interrupt-Kanal geschlossen.\n");
                        goto shutdown_link;
                    } else {
                        printf("vDS-Proxy: [INTR OUT] %d Bytes -> Controller\n", len);
                        send(client_intr, heap_buffer, len, 0);
                    }
                }
            }
            continue;
            
        shutdown_link:
            printf("vDS-Proxy: Pipeline-Verbindung getrennt. Setze Routing-Infrastruktur zurueck...\n");
            
            // FDs sicher schliessen, falls sie geoeffnet sind
            if (client_ctrl >= 0) close(client_ctrl);
            if (client_intr >= 0) close(client_intr);
            if (vdsd_ctrl >= 0) close(vdsd_ctrl);
            if (vdsd_intr >= 0) close(vdsd_intr);
            
            // Zustandsvariablen fuer den naechsten Handshake-Versuch zuruecksetzen
            client_ctrl = -1;
            client_intr = -1;
            vdsd_ctrl = -1;
            vdsd_intr = -1;
            state = 0;
        }
    }
    
    // Aufraeumen vor dem Beenden (wird im endlosen Loop nie erreicht, gehoert zum sauberen C-Stil)
    free(heap_buffer);
    return 0;
}
