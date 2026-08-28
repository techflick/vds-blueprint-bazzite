#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <errno.h>

#define AF_BLUETOOTH       31
#define SOCK_SEQPACKET     5
#define BTPROTO_L2CAP      0

// Das vom Host-Kernel (Bazzite) geforderte reale 16-Byte Layout
struct sockaddr_l2_local {
    sa_family_t l2_family;      // 2 Byte
    uint16_t    l2_psm;         // 2 Byte
    uint8_t     l2_bdaddr[6];   // 6 Byte
    uint16_t    l2_cid;         // 2 Byte
    uint8_t     l2_bdaddr_type; // 1 Byte
    uint8_t     padding[3];     // 3 Byte -> Ergibt exakt 16 Byte
};

// Erstellt den Server-Socket für eingehende Controller-Verbindungen
int create_server_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_l2_local addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;
    
    // Zwingt den Kernel auf die exakten 16 Byte des Hosts
    if (bind(sock, (struct sockaddr *)&addr, 16) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, 5) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

// Baut die Weiterleitung zum via Regex gepatchten vdsd-Daemon auf
int connect_to_vdsd(uint16_t psm, uint8_t *mac) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    struct sockaddr_l2_local addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;
    
    // Übergabe der gespiegelten MAC-Adresse an den Daemon
    memcpy(addr.l2_bdaddr, mac, 6);
    addr.l2_bdaddr_type = 0;
    
    // Zwingt den Connect auf die reale Host-Größe von 16 Byte gegen EINVAL
    if (connect(sock, (struct sockaddr *)&addr, 16) < 0) {
        fprintf(stderr, "vDS-Proxy DEBUG: connect zu vdsd (Port 0x%04X) fehlgeschlagen: %s\n", psm, strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    printf("vDS-Proxy: Starte Socket-Routing-Infrastruktur...\n");

    int srv_ctrl = create_server_socket(0x11);
    int srv_intr = create_server_socket(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) {
        fprintf(stderr, "vDS-Proxy KRITISCH: Server Sockets konnten nicht erstellt werden.\n");
        return 1;
    }

    struct pollfd srv_fds;
    srv_fds.fd = srv_ctrl; 
    srv_fds.events = POLLIN; 
    srv_fds.revents = 0;

    int client_ctrl = -1, client_intr = -1;
    int vdsd_ctrl = -1, vdsd_intr = -1;
    uint8_t target_mac[6] = {0};

    printf("vDS-Proxy: Warte auf Control-Kanal (PSM 0x11)...\n");

    // 1. Control-Kanal (0x11) abfangen & MAC isolieren
    while (poll(&srv_fds, 1, -1) > 0) {
        if (srv_fds.revents & POLLIN) {
            // Großer, neutraler Speicherblock fängt Layout-Schwankungen des Kernels ab
            uint8_t sockaddr_storage[64]; 
            memset(sockaddr_storage, 0, sizeof(sockaddr_storage));
            socklen_t addr_len = sizeof(sockaddr_storage);
            
            // FIX: Nutzt jetzt korrekt den deklarierten Speicherblock
            client_ctrl = accept(srv_ctrl, (struct sockaddr *)sockaddr_storage, &addr_len);
            if (client_ctrl >= 0) {
                struct sockaddr_l2_local *raw_addr = (struct sockaddr_l2_local *)sockaddr_storage;
                
                // MAC-Endianness korrigieren: Kernel (Little-Endian) zu Daemon (Big-Endian) spiegeln
                target_mac[0] = raw_addr->l2_bdaddr[5];
                target_mac[1] = raw_addr->l2_bdaddr[4];
                target_mac[2] = raw_addr->l2_bdaddr[3];
                target_mac[3] = raw_addr->l2_bdaddr[2];
                target_mac[4] = raw_addr->l2_bdaddr[1];
                target_mac[5] = raw_addr->l2_bdaddr[0];
                
                printf("vDS-Proxy: Control-Kanal verbunden. MAC (korrigiert): %02X:%02X:%02X:%02X:%02X:%02X\n",
                       target_mac[0], target_mac[1], target_mac[2],
                       target_mac[3], target_mac[4], target_mac[5]);
                
                // Protokoll-Spionage via MSG_PEEK
                uint8_t peek_buf[1];
                recv(client_ctrl, peek_buf, sizeof(peek_buf), MSG_PEEK | MSG_DONTWAIT);
                
                // Verbindet sofort weiter zu vdsd auf Ausweich-Port 0x0021
                vdsd_ctrl = connect_to_vdsd(0x0021, target_mac);
                if (vdsd_ctrl < 0) {
                    fprintf(stderr, "vDS-Proxy: Weiterleitung zu vdsd auf Port 0x0021 fehlgeschlagen.\n");
                }
            }
            break;
        }
    }

    printf("vDS-Proxy: Warte auf Interrupt-Kanal (PSM 0x13)...\n");

    struct pollfd srv_intr_fd;
    srv_intr_fd.fd = srv_intr; 
    srv_intr_fd.events = POLLIN; 
    srv_intr_fd.revents = 0;

    while (poll(&srv_intr_fd, 1, 5000) > 0) { // 5 Sekunden Timeout
        if (srv_intr_fd.revents & POLLIN) {
            uint8_t sockaddr_storage[64];
            memset(sockaddr_storage, 0, sizeof(sockaddr_storage));
            socklen_t addr_len = sizeof(sockaddr_storage);
            
            client_intr = accept(srv_intr, (struct sockaddr *)sockaddr_storage, &addr_len);
            if (client_intr >= 0) {
                printf("vDS-Proxy: Interrupt-Kanal verbunden.\n");
                
                // Verbindet sofort weiter zu vdsd auf Ausweich-Port 0x0023
                vdsd_intr = connect_to_vdsd(0x0023, target_mac);
                if (vdsd_intr < 0) {
                    fprintf(stderr, "vDS-Proxy: Weiterleitung zu vdsd auf Port 0x0023 fehlgeschlagen.\n");
                }
            }
            break;
        }
    }

    // Falls der Tunnel-Aufbau fehlschlug, sauber abbrechen
    if (client_ctrl < 0 || client_intr < 0 || vdsd_ctrl < 0 || vdsd_intr < 0) {
        fprintf(stderr, "vDS-Proxy KRITISCH: Tunnel unvollständig. Teardown eingeleitet.\n");
        if (client_ctrl >= 0) close(client_ctrl);
        if (client_intr >= 0) close(client_intr);
        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        if (vdsd_intr >= 0) close(vdsd_intr);
        close(srv_ctrl); close(srv_intr);
        return 1;
    }

    printf("vDS-Proxy: **Bidirektionaler Userspace-Tunnel aktiv!**\n");

    struct pollfd tunnel_fds[4];
    uint8_t buf[2048];

    while (1) {
        // Korrekte Zuweisung des gesamten Arrays vor jedem Durchlauf
        tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN; tunnel_fds[0].revents = 0;
        tunnel_fds[1].fd = vdsd_ctrl;   tunnel_fds[1].events = POLLIN; tunnel_fds[1].revents = 0;
        tunnel_fds[2].fd = client_intr; tunnel_fds[2].events = POLLIN; tunnel_fds[2].revents = 0;
        tunnel_fds[3].fd = vdsd_intr;   tunnel_fds[3].events = POLLIN; tunnel_fds[3].revents = 0;

        if (poll(tunnel_fds, 4, -1) <= 0) continue;

        // Daten vom Controller zu vdsd (Control)
        if (tunnel_fds[0].revents & POLLIN) {
            int len = recv(client_ctrl, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(vdsd_ctrl, buf, len, 0);
        }
        // Daten von vdsd zum Controller (Control)
        if (tunnel_fds[1].revents & POLLIN) {
            int len = recv(vdsd_ctrl, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(client_ctrl, buf, len, 0);
        }
        // Daten vom Controller zu vdsd (Interrupt)
        if (tunnel_fds[2].revents & POLLIN) {
            int len = recv(client_intr, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(vdsd_intr, buf, len, 0);
        }
        // Daten von vdsd zum Controller (Interrupt)
        if (tunnel_fds[3].revents & POLLIN) {
            int len = recv(vdsd_intr, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(client_intr, buf, len, 0);
        }
        
        // Signalverlust-Schutz
        int disconnect = 0;
        for (int i = 0; i < 4; i++) {
            if (tunnel_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                printf("vDS-Proxy: Signalverlust auf Socket-Index %d.\n", i);
                disconnect = 1;
                break;
            }
        }
        if (disconnect) break;
    }

    printf("vDS-Proxy: Tunnel beendet. Schließe Sockets...\n");
    close(client_ctrl); close(client_intr); close(vdsd_ctrl); close(vdsd_intr);
    close(srv_ctrl); close(srv_intr);
    return 0;
}
