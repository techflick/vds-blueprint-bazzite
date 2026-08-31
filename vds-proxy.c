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
    
    // Starre 16-Byte-Größe für echten Bluetooth-Kontext des Proxys erzwingen
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

int connect_unix_pipe(const char *full_name) {
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock < 0) return -1;
    
    // Wir bauen das exakte 14-Byte-Speicherabbild des Daemons nach
    unsigned char raw_addr[14];
    memset(raw_addr, 0, 14);
    
    // Byte 0-1: AF_UNIX (Wert 1) im Little-Endian-Format
    raw_addr[0] = 1;
    raw_addr[1] = 0;
    
    // Byte 2: Ist das \0 für den abstrakten Raum (durch memset bereits 0)
    
    // Byte 3-5: Kopiert "v_c" oder "v_i" exakt an Position 3
    // full_name enthält im Hauptprogramm "v_c\0..." -> wir greifen nur die ersten 3 Zeichen
    memcpy(&raw_addr[3], full_name, 3);
    
    // Byte 6-13: Bleiben durch das memset oben exakt Nullbytes
    // Das ergibt im RAM genau das vom Daemon erzeugte "@v_c@@@@@@@@"
    
    // CRITICAL MATCH: Wir erzwingen beim Connect die identische Länge von 14 Bytes!
    int len = 14;

    // Blockierend verbinden für stabilen Handshake vor dem Loop
    if (connect(sock, (struct sockaddr *)raw_addr, len) < 0) {
        close(sock);
        return -1;
    }
    
    // Danach erst auf Non-Blocking für die High-Speed-Pipeline schalten
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
            // Explizite Indizierung der srv_fds zur Vermeidung von Deskriptor-Speicherfehlern
            struct pollfd srv_fds[2];
            memset(srv_fds, 0, sizeof(srv_fds));
            srv_fds[0].fd = srv_ctrl; srv_fds[0].events = POLLIN;
            srv_fds[1].fd = srv_intr; srv_fds[1].events = POLLIN;

            int ret = poll(srv_fds, 2, 100);
            if (ret > 0) {
                if (srv_fds[0].revents & POLLIN) {
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
                if (srv_fds[1].revents & POLLIN) {
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
            }

            if (client_ctrl >= 0 && client_intr >= 0) {
                printf("vDS-Proxy: Reiche Daten ueber RAM-Sockets an den Daemon weiter...\n");
                
                // Wir übergeben den Namen exakt so, wie er vom vdsd im RAM angelegt wird:
                // "v_c" bzw. "v_i" + 8-mal das Nullbyte '\0' (Gesamtlänge im abstrakten Raum = 11 Byte)
                vdsd_ctrl = connect_unix_pipe("v_c\0\0\0\0\0\0\0\0");
                vdsd_intr = connect_unix_pipe("v_i\0\0\0\0\0\0\0\0");

                if (vdsd_ctrl >= 0 && vdsd_intr >= 0) {
                    printf("vDS-Proxy: **Latenzfreie Speicher-Pipeline aktiv!**\n");
                    state = 1;
                    continue;
                }
                goto shutdown_link;
            }
        } else {
            // Alle 4 Deskriptoren gebündelt in einem gemeinsamen pollfd-Array
            struct pollfd tunnel_fds[4];
            memset(tunnel_fds, 0, sizeof(tunnel_fds));
            tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN;
            tunnel_fds[1].fd = vdsd_ctrl;   tunnel_fds[1].events = POLLIN;
            tunnel_fds[2].fd = client_intr; tunnel_fds[2].events = POLLIN;
            tunnel_fds[3].fd = vdsd_intr;   tunnel_fds[3].events = POLLIN;

            // Kurzer Timeout für die High-Speed-Pipeline
            int ret = poll(tunnel_fds, 4, 10);

            if (ret > 0) {
                // Jedes Event EINZELN prüfen statt globaler ODER-Verknüpfung
                for (int i = 0; i < 4; i++) {
                    if (tunnel_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                        goto shutdown_link;
                    }
                }

                // Control-Kanal: Controller -> Daemon
                if (tunnel_fds[0].revents & POLLIN) {
                    int len = recv(client_ctrl, heap_buffer, 1024, 0);
                    if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    if (len > 0) send(vdsd_ctrl, heap_buffer, len, 0);
                }
                // Control-Kanal: Daemon -> Controller
                if (tunnel_fds[1].revents & POLLIN) {
                    int len = recv(vdsd_ctrl, heap_buffer, 1024, 0);
                    if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    if (len > 0) send(client_ctrl, heap_buffer, len, 0);
                }
                // Interrupt-Kanal: Controller -> Daemon
                if (tunnel_fds[2].revents & POLLIN) {
                    int len = recv(client_intr, heap_buffer, 1024, 0);
                    if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    if (len > 0) send(vdsd_intr, heap_buffer, len, 0);
                }
                // Interrupt-Kanal: Daemon -> Controller
                if (tunnel_fds[3].revents & POLLIN) {
                    int len = recv(vdsd_intr, heap_buffer, 1024, 0);
                    if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto shutdown_link;
                    if (len > 0) send(client_intr, heap_buffer, len, 0);
                }
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
