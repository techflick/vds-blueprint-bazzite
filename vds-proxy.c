#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <errno.h>

// [1.1] Absolut autarke Kernel-Definitionen – Unabhängig von Daemon-Headern
#define AF_BLUETOOTH       31
#define SOCK_SEQPACKET     5
#define BTPROTO_L2CAP      0  // Native Protokoll-ID 0 gegen Kernel-Blockaden

// [1.1] Das vom modernen Bazzite-Host zwingend geforderte 16-Byte Layout
struct sockaddr_l2_local {
    sa_family_t l2_family;      // 2 Byte
    uint16_t    l2_psm;         // 2 Byte
    uint8_t     l2_bdaddr[6];   // 6 Byte (Reale Bluetooth MAC-Adresse)
    uint16_t    l2_cid;         // 2 Byte
    uint8_t     l2_bdaddr_type; // 1 Byte
    uint8_t     padding[3];     // 3 Byte -> Ergibt exakt die 16-Byte-Grenze
};

// Erstellt den Server-Socket für eingehende Controller-Verbindungen
int create_server_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_l2_local addr;
    memset(&addr, 0, sizeof(addr)); // WICHTIG: Nullt das Padding gegen EINVAL
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm; // 0x11 (Control) oder 0x13 (Interrupt)
    
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

// Baut die Userspace-Weiterleitung zum gepatchten vdsd-Daemon auf
int connect_to_vdsd(uint16_t psm, uint8_t *mac) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    struct sockaddr_l2_local addr;
    memset(&addr, 0, sizeof(addr)); // WICHTIG: Keine Stack-Reste ins Kernel-Routing
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm; // 0x0021 oder 0x0023
    memcpy(addr.l2_bdaddr, mac, 6); // Dynamische MAC-Zuweisung [I]
    addr.l2_bdaddr_type = 0;
    
    // Zwingt den Connect auf die reale Host-Größe von 16 Byte [I]
    if (connect(sock, (struct sockaddr *)&addr, 16) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    printf("vDS-Proxy: Starte Socket-Routing-Infrastruktur...\n");

    // Sockets für die eingehenden Controller-Kanäle öffnen
    int srv_ctrl = create_server_socket(0x11);
    int srv_intr = create_server_socket(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) {
        fprintf(stderr, "vDS-Proxy KRITISCH: Server Sockets konnten nicht erstellt werden.\n");
        return 1;
    }

    struct pollfd srv_fds[2];
    srv_fds[0].fd = srv_ctrl; srv_fds[0].events = POLLIN; srv_fds[0].revents = 0;
    srv_fds[1].fd = srv_intr; srv_fds[1].events = POLLIN; srv_fds[1].revents = 0;

    int client_ctrl = -1, client_intr = -1;
    int vdsd_ctrl = -1, vdsd_intr = -1;
    uint8_t target_mac[6] = {0};

    printf("vDS-Proxy: Warte auf Control-Kanal (PSM 0x11)...\n");

    // 1. Abfangen des Control-Kanals (0x11) & Dynamisches Auslesen der Controller-MAC
    while (poll(srv_fds, 1, -1) > 0) { // Nur auf Control-Server horchen
        if (srv_fds[0].revents & POLLIN) {
            struct sockaddr_l2_local raw_addr;
            memset(&raw_addr, 0, sizeof(raw_addr));
            socklen_t addr_len = sizeof(raw_addr); 
            
            // Absturzsicherer Accept fängt Kernel-Layouts sauber ab [I]
            client_ctrl = accept(srv_ctrl, (struct sockaddr *)&raw_addr, &addr_len);
            if (client_ctrl >= 0) {
                // MAC isolieren, um Strukturänderungen des Kernels auszuhebeln [I]
                memcpy(target_mac, raw_addr.l2_bdaddr, 6);
                
                printf("vDS-Proxy: Control-Kanal verbunden. MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       target_mac[0], target_mac[1], target_mac[2],
                       target_mac[3], target_mac[4], target_mac[5]);
                
                // [WICHTIG] Protokoll-Spionage via MSG_PEEK
                // Liest zerstörungsfrei, damit das Erstpaket in der Kernel-Queue bleibt
                uint8_t peek_buf[1];
                recv(client_ctrl, peek_buf, sizeof(peek_buf), MSG_PEEK | MSG_DONTWAIT);
                
                // Verbindet den Proxy sofort weiter zu vdsd auf Ausweich-Port 0x0021 [I]
                vdsd_ctrl = connect_to_vdsd(0x0021, target_mac);
                if (vdsd_ctrl < 0) {
                    fprintf(stderr, "vDS-Proxy: Weiterleitung zu vdsd auf Port 0x0021 fehlgeschlagen: %s\n", strerror(errno));
                }
            }
            break;
        }
    }

    printf("vDS-Proxy: Warte auf Interrupt-Kanal (PSM 0x13)...\n");

    // 2. Isolierte Struktur für Interrupt-Kanal, um Kernel-Überschreibungen zu verhindern
    struct pollfd srv_intr_fd;
    srv_intr_fd.fd = srv_intr; srv_intr_fd.events = POLLIN; srv_intr_fd.revents = 0;

    while (poll(&srv_intr_fd, 1, 5000) > 0) { // 5 Sekunden Timeout für den zweiten Kanal
        if (srv_intr_fd.revents & POLLIN) {
            struct sockaddr_l2_local raw_addr;
            memset(&raw_addr, 0, sizeof(raw_addr));
            socklen_t addr_len = sizeof(raw_addr);
            
            client_intr = accept(srv_intr, (struct sockaddr *)&raw_addr, &addr_len);
            if (client_intr >= 0) {
                printf("vDS-Proxy: Interrupt-Kanal verbunden.\n");
                
                // Verbindet den Proxy sofort weiter zu vdsd auf Ausweich-Port 0x0023 [I]
                vdsd_intr = connect_to_vdsd(0x0023, target_mac);
                if (vdsd_intr < 0) {
                    fprintf(stderr, "vDS-Proxy: Weiterleitung zu vdsd auf Port 0x0023 fehlgeschlagen: %s\n", strerror(errno));
                }
            }
            break;
        }
    }

    // Falls der Tunnel-Aufbau unvollständig war, Sockets schließen und abbrechen
    if (client_ctrl < 0 || client_intr < 0 || vdsd_ctrl < 0 || vdsd_intr < 0) {
        fprintf(stderr, "vDS-Proxy KRITISCH: Tunnel unvollständig. Teardown eingeleitet.\n");
        if (client_ctrl >= 0) close(client_ctrl);
        if (client_intr >= 0) close(client_intr);
        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        if (vdsd_intr >= 0) close(vdsd_intr);
        close(srv_ctrl); close(srv_intr);
        return 1;
    }

    printf("vDS-Proxy: **Bidirektionaler Userspace-Tunnel aktiv!** Datenübertragung läuft...\n");

    // 3. Der asynchrone Userspace-Tunnel (Reicht alle Daten ungezählt im Kreis weiter)
    struct pollfd tunnel_fds[4];
    tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN; tunnel_fds[0].revents = 0;
    tunnel_fds[1].fd = vdsd_ctrl;   tunnel_fds[1].events = POLLIN; tunnel_fds[1].revents = 0;
    tunnel_fds[2].fd = client_intr; tunnel_fds[2].events = POLLIN; tunnel_fds[2].revents = 0;
    tunnel_fds[3].fd = vdsd_intr;   tunnel_fds[3].events = POLLIN; tunnel_fds[3].revents = 0;

    uint8_t buf[2048]; // Erhöht auf 2048 Byte für haptische Audio-Spitzen
    while (poll(tunnel_fds, 4, -1) > 0) {
        // Daten vom Controller zu vdsd (Control-Kanal)
        // HIER wird das spionierte Erstpaket jetzt regulär abgeholt und weitergereicht
        if (tunnel_fds[0].revents & POLLIN) {
            int len = recv(client_ctrl, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(vdsd_ctrl, buf, len, 0);
        }
        // Daten von vdsd zum Controller (Control-Kanal - z.B. Licht/Haptik)
        if (tunnel_fds[1].revents & POLLIN) {
            int len = recv(vdsd_ctrl, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(client_ctrl, buf, len, 0);
        }
        // Daten vom Controller zu vdsd (Interrupt-Kanal - Eingaben/Tasten)
        if (tunnel_fds[2].revents & POLLIN) {
            int len = recv(client_intr, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(vdsd_intr, buf, len, 0);
        }
        // Daten von vdsd zum Controller (Interrupt-Kanal)
        if (tunnel_fds[3].revents & POLLIN) {
            int len = recv(vdsd_intr, buf, sizeof(buf), 0);
            if (len <= 0) break;
            send(client_intr, buf, len, 0);
        }
        
        // Anti-Freeze & Verbindungsabbruch-Schutz
        for (int i = 0; i < 4; i++) {
            if (tunnel_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                printf("vDS-Proxy: Signalverlust auf Socket-Index %d.\n", i);
                goto cleanup;
            }
        }
    }

cleanup:
    printf("vDS-Proxy: Verbindung beendet. Schließe Tunnel Sockets...\n");

    // Sauberes Teardown bei Verbindungsabbruch
    close(client_ctrl); close(client_intr); close(vdsd_ctrl); close(vdsd_intr);
    close(srv_ctrl); close(srv_intr);
    return 0;
}
