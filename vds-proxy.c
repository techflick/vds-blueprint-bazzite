#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
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

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_server_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (set_nonblocking(sock) < 0) {
        close(sock);
        return -1;
    }
    
    struct sockaddr_l2_local addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;
    
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

int connect_to_vdsd(uint16_t psm, uint8_t *mac) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    if (set_nonblocking(sock) < 0) {
        close(sock);
        return -1;
    }
    
    struct sockaddr_l2_local addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;
    
    memcpy(addr.l2_bdaddr, mac, 6);
    addr.l2_bdaddr_type = 0;
    
    int res = connect(sock, (struct sockaddr *)&addr, 16);
    if (res < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    printf("vDS-Proxy: Starte boot-sichere Routing-Infrastruktur...\n");

    int srv_ctrl = create_server_socket(0x11);
    int srv_intr = create_server_socket(0x13);
    if (srv_ctrl < 0 || srv_intr < 0) {
        fprintf(stderr, "vDS-Proxy KRITISCH: Sockets blockiert.\n");
        return 1;
    }

    int client_ctrl = -1, client_intr = -1;
    int vdsd_ctrl = -1, vdsd_intr = -1;
    uint8_t target_mac[6] = {0};
    int state = 0; 

    printf("vDS-Proxy: Initialisierung erfolgreich. Warte auf Geräte...\n");

    while (1) {
        if (state == 0) {
            struct pollfd srv_fds[2];
            srv_fds[0].fd = srv_ctrl; srv_fds[0].events = POLLIN; srv_fds[0].revents = 0;
            srv_fds[1].fd = srv_intr; srv_fds[1].events = POLLIN; srv_fds[1].revents = 0;

            int ret = poll(srv_fds, 2, 1000); 
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;

            // 1. Control-Kanal (PSM 0x11) nimmt Verbindung an
            if (srv_fds[0].revents & POLLIN) {
                uint8_t sockaddr_storage[64];
                socklen_t addr_len = sizeof(sockaddr_storage);
                int tmp_client = accept(srv_ctrl, (struct sockaddr *)sockaddr_storage, &addr_len);
                
                if (tmp_client >= 0) {
                    if (client_ctrl >= 0) close(client_ctrl); 
                    client_ctrl = tmp_client;
                    set_nonblocking(client_ctrl);
                    struct sockaddr_l2_local *raw_addr = (struct sockaddr_l2_local *)sockaddr_storage;
                    
                    // WICHTIGER ARCHITEKTUR-FOKUS: Invertiert die MAC, da vdsd sie gedreht erwartet!
                    for(int i = 0; i < 6; i++) {
                        target_mac[i] = raw_addr->l2_bdaddr[5 - i];
                    }
                    
                    printf("vDS-Proxy: Control-Kanal aktiv. Invertierte Ziel-MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                           target_mac[0], target_mac[1], target_mac[2], target_mac[3], target_mac[4], target_mac[5]);
                    
                    uint8_t peek_buf[1];
                    recv(client_ctrl, peek_buf, sizeof(peek_buf), MSG_PEEK | MSG_DONTWAIT);
                }
            }

            // 2. Interrupt-Kanal (PSM 0x13) nimmt Verbindung an
            if (srv_fds[1].revents & POLLIN) {
                uint8_t sockaddr_storage[64];
                socklen_t addr_len = sizeof(sockaddr_storage);
                int tmp_client = accept(srv_intr, (struct sockaddr *)sockaddr_storage, &addr_len);
                
                if (tmp_client >= 0) {
                    if (client_intr >= 0) close(client_intr); 
                    client_intr = tmp_client;
                    set_nonblocking(client_intr);
                    printf("vDS-Proxy: Interrupt-Kanal aktiv.\n");
                }
            }

            // Erst wenn beide physischen Kanäle bereitstehen, wird vdsd gekoppelt
            if (client_ctrl >= 0 && client_intr >= 0) {
                vdsd_ctrl = connect_to_vdsd(0x0021, target_mac);
                vdsd_intr = connect_to_vdsd(0x0023, target_mac);

                if (vdsd_ctrl >= 0 && vdsd_intr >= 0) {
                    printf("vDS-Proxy: **Bidirektionaler Userspace-Tunnel aktiv!**\n");
                    state = 1;
                } else {
                    fprintf(stderr, "vDS-Proxy: Weiterleitung an vdsd fehlgeschlagen. Setze Kanäle zurück.\n");
                    goto clean_disconnect;
                }
            }
        } else {
            struct pollfd tunnel_fds[4];
            uint8_t buf[2048];

            tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN; tunnel_fds[0].revents = 0;
            tunnel_fds[1].fd = vdsd_ctrl;   tunnel_fds[1].events = POLLIN; tunnel_fds[1].revents = 0;
            tunnel_fds[2].fd = client_intr; tunnel_fds[2].events = POLLIN; tunnel_fds[2].revents = 0;
            tunnel_fds[3].fd = vdsd_intr;   tunnel_fds[3].events = POLLIN; tunnel_fds[3].revents = 0;

            int ret = poll(tunnel_fds, 4, 500);
            
            if (ret < 0) {
                if (errno == EINTR) continue;
                goto clean_disconnect;
            }

            for (int i = 0; i < 4; i++) {
                if (tunnel_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    goto clean_disconnect;
                }
            }

            if (ret == 0) continue;

            // Daten-Weichen-Routing (Control)
            if (tunnel_fds[0].revents & POLLIN) {
                int len = recv(client_ctrl, buf, sizeof(buf), 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    goto clean_disconnect;
                }
                send(vdsd_ctrl, buf, len, 0);
            }
            if (tunnel_fds[1].revents & POLLIN) {
                int len = recv(vdsd_ctrl, buf, sizeof(buf), 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    goto clean_disconnect;
                }
                send(client_ctrl, buf, len, 0);
            }
            
            // Daten-Weichen-Routing (Interrupt)
            if (tunnel_fds[2].revents & POLLIN) {
                int len = recv(client_intr, buf, sizeof(buf), 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    goto clean_disconnect;
                }
                send(vdsd_intr, buf, len, 0);
            }
            if (tunnel_fds[3].revents & POLLIN) {
                int len = recv(vdsd_intr, buf, sizeof(buf), 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    goto clean_disconnect;
                }
                send(client_intr, buf, len, 0);
            }
            continue;

        clean_disconnect:
            printf("vDS-Proxy: Verbindung verloren / Controller abgeschaltet. Setze Tunnel zurück...\n");
            if (client_ctrl >= 0) close(client_ctrl);
            if (client_intr >= 0) close(client_intr);
            if (vdsd_ctrl >= 0) close(vdsd_ctrl);
            if (vdsd_intr >= 0) close(vdsd_intr);
            client_ctrl = client_intr = vdsd_ctrl = vdsd_intr = -1;
            state = 0; 
        }
    }

    close(srv_ctrl);
    close(srv_intr);
    return 0;
}
