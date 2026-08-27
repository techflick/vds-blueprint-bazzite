#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <errno.h>

// [1.1] Absolut autarke Kernel-Definitionen – Unabhängig von externen Headern
#define AF_BLUETOOTH       31
#define SOCK_SEQPACKET     5
#define BTPROTO_L2CAP      0  // WICHTIG: Native 0, damit der Kernel nicht blockiert

// [1.1] Das vom modernen Bazzite-Host zwingend geforderte 16-Byte Layout
struct sockaddr_l2_local {
    sa_family_t l2_family;      // 2 Byte
    uint16_t    l2_psm;         // 2 Byte
    uint8_t     l2_bdaddr[6];   // 6 Byte (Reale Bluetooth MAC-Adresse)
    uint16_t    l2_cid;         // 2 Byte
    uint8_t     l2_bdaddr_type; // 1 Byte
    uint8_t     padding[3];     // 3 Byte -> Ergibt exakt die 16-Byte-Partitionsgrenze
};

// Erstellt den Server-Socket, der auf Verbindungen des physischen Controllers wartet
int create_server_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_l2_local addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm; // 0x11 (Control) oder 0x13 (Interrupt)
    
    // Zwingt den Kernel auf die exakten 16 Byte des Hosts [I]
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

// Baut die Userspace-Weiterleitung (Connect) zum umgebogenen vdsd-Daemon auf
int connect_to_vdsd(uint16_t psm, uint8_t *mac) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    struct sockaddr_l2_local addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm; // 0x0021 oder 0x0023
    memcpy(addr.l2_bdaddr, mac, 6); // Übergibt die dynamische MAC des Controllers für das Kernel-Routing [I]
    
    if (connect(sock, (struct sockaddr *)&addr, 16) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    // Sockets für die eingehenden Controller-Kanäle öffnen
    int srv_ctrl = create_server_socket(0x11);
    int srv_intr = create_server_socket(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) return 1;

    struct pollfd srv_fds[2];
    srv_fds[0].fd = srv_ctrl; srv_fds[0].events = POLLIN;
    srv_fds[1].fd = srv_intr; srv_fds[1].events = POLLIN;

    int client_ctrl = -1, client_intr = -1;
    int vdsd_ctrl = -1, vdsd_intr = -1;
    
    struct sockaddr_l2_local client_addr = {0};
    socklen_t addr_len = sizeof(client_addr);

    // 1. Abfangen des Control-Kanals (0x11) & Dynamisches Auslesen der Controller-MAC [I]
    while (poll(srv_fds, 2, -1) > 0) {
        if (srv_fds[0].revents & POLLIN) {
            client_ctrl = accept(srv_ctrl, (struct sockaddr *)&client_addr, &addr_len);
            if (client_ctrl >= 0) {
                // Verbindet den Proxy sofort weiter zu vdsd auf den Ausweich-Port 0x0021
                vdsd_ctrl = connect_to_vdsd(0x0021, client_addr.l2_bdaddr);
            }
            break;
        }
    }

    // 2. Abfangen des Interrupt-Kanals (0x13) innerhalb eines 5-Sekunden-Fensters
    srv_fds[1].fd = srv_intr;
    while (poll(&srv_fds[1], 1, 5000) > 0) {
        if (srv_fds[1].revents & POLLIN) {
            client_intr = accept(srv_intr, NULL, NULL);
            if (client_intr >= 0) {
                // Verbindet den Proxy sofort weiter zu vdsd auf den Ausweich-Port 0x0023
                vdsd_intr = connect_to_vdsd(0x0023, client_addr.l2_bdaddr);
            }
            break;
        }
    }

    // Falls der Tunnel-Aufbau unvollständig war, Sockets schließen und abbrechen
    if (client_ctrl < 0 || client_intr < 0 || vdsd_ctrl < 0 || vdsd_intr < 0) {
        if (client_ctrl >= 0) close(client_ctrl);
        if (client_intr >= 0) close(client_intr);
        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
        if (vdsd_intr >= 0) close(vdsd_intr);
        close(srv_ctrl); close(srv_intr);
        return 1;
    }

    // 3. Der bidirektionale Userspace-Tunnel (Reicht alle Daten ungezählt weiter) [I]
    struct pollfd tunnel_fds[4];
    tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN;
    tunnel_fds[1].fd = vdsd_ctrl;   tunnel_fds[1].events = POLLIN;
    tunnel_fds[2].fd = client_intr; tunnel_fds[2].events = POLLIN;
    tunnel_fds[3].fd = vdsd_intr;   tunnel_fds[3].events = POLLIN;

    uint8_t buf[1024];
    while (poll(tunnel_fds, 4, -1) > 0) {
        // Daten vom Controller zu vdsd (Control-Kanal)
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
    }

    // Sauberes Teardown bei Verbindungsabbruch
    close(client_ctrl); close(client_intr); close(vdsd_ctrl); close(vdsd_intr);
    close(srv_ctrl); close(srv_intr);
    return 0;
}
