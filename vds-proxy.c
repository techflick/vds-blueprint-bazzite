#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/uhid.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#define BTPROTO_L2CAP 0
#define BUFFER_SIZE 1024

struct sockaddr_l2_local {
    uint16_t l2_family;
    uint16_t l2_psm;
    uint8_t  l2_bdaddr[6];
    uint16_t l2_cid;
    uint8_t  l2_bdaddr_type;
};

static unsigned char ds4_bt_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_l2cap_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_l2_local addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    listen(sock, 5);
    set_nonblocking(sock);
    return sock;
}

// Baut die ausgehende Verbindung zum gepatchten vdsd-Daemon auf
int connect_to_vdsd(uint16_t target_psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;

    struct sockaddr_l2_local addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = target_psm; // Verbindet zu 0x21 oder 0x23

    // Da vdsd lokal auf Verbindungen wartet, reicht ein Connect auf die Any-Struktur
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    set_nonblocking(sock);
    return sock;
}

int main() {
    int server_ctrl = create_l2cap_socket(0x11);
    int server_int = create_l2cap_socket(0x13);
    
    if (server_ctrl < 0 || server_int < 0) return 1;

    struct pollfd s_fds[2];
    s_fds[0].fd = server_ctrl;
    s_fds[0].events = POLLIN;
    s_fds[1].fd = server_int;
    s_fds[1].events = POLLIN;

    while (1) {
        int ret = poll(s_fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (s_fds[0].revents & POLLIN) {
            struct sockaddr_l2_local client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_ctrl = accept(server_ctrl, (struct sockaddr *)&client_addr, &addr_len);
            
            if (client_ctrl >= 0) {
                set_nonblocking(client_ctrl);
                char peek_buf[BUFFER_SIZE] = {0};
                usleep(15000); 
                ssize_t peek_len = recv(client_ctrl, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);

                int is_dualsense = 0;
                if (peek_len > 0 && (strstr(peek_buf, "DualSense") != NULL || peek_buf[0] == 0x01)) {
                    is_dualsense = 1;
                }

                /* ========================================================
                   FALL 1: DUALSENSE - TRANSPARENTES USERSPACE-FORWARDING
                   ======================================================== */
                if (is_dualsense) {
                    printf("[+] DualSense erkannt. Initialisiere Weiche zu vdsd...\n");
                    
                    // Hol dir sofort das Gegenstück des Interrupt-Kanals vom Controller
                    struct pollfd int_check;
                    int_check.fd = server_int;
                    int_check.events = POLLIN;
                    int client_int = -1;
                    
                    if (poll(&int_check, 1, 1500) > 0 && (int_check.revents & POLLIN)) {
                        client_int = accept(server_int, (struct sockaddr *)&client_addr, &addr_len);
                    }

                    if (client_int < 0) {
                        close(client_ctrl);
                        continue;
                    }
                    set_nonblocking(client_int);

                    // Verbinde den Proxy mit dem im Hintergrund wartenden vdsd (Ports 0x21 und 0x23)
                    int vdsd_ctrl = connect_to_vdsd(0x21);
                    int vdsd_int = connect_to_vdsd(0x23);

                    if (vdsd_ctrl < 0 || vdsd_int < 0) {
                        printf("[-] Verbindung zu vdsd fehlgeschlagen. Läuft der Daemon?\n");
                        close(client_ctrl);
                        close(client_int);
                        if (vdsd_ctrl >= 0) close(vdsd_ctrl);
                        continue;
                    }

                    printf("[+] Tunnel zu vdsd steht. Starte DualSense Datenbrücke.\n");

                    struct pollfd ds_fds[4];
                    ds_fds[0].fd = client_ctrl;  ds_fds[0].events = POLLIN;
                    ds_fds[1].fd = client_int;   ds_fds[1].events = POLLIN;
                    ds_fds[2].fd = vdsd_ctrl;    ds_fds[2].events = POLLIN;
                    ds_fds[3].fd = vdsd_int;     ds_fds[3].events = POLLIN;

                    unsigned char io_buf[BUFFER_SIZE];
                    int active = 1;

                    while (active) {
                        int p_ret = poll(ds_fds, 4, -1);
                        if (p_ret < 0) {
                            if (errno == EINTR) continue;
                            break;
                        }

                        // Verbindungstrennung prüfen
                        for (int k = 0; k < 4; k++) {
                            if (ds_fds[k].revents & (POLLHUP | POLLERR)) active = 0;
                        }
                        if (!active) break;

                        // Controller Control -> vdsd Control
                        if (ds_fds[0].revents & POLLIN) {
                            ssize_t len = read(client_ctrl, io_buf, sizeof(io_buf));
                            if (len <= 0) break;
                            write(vdsd_ctrl, io_buf, len);
                        }
                        // Controller Interrupt -> vdsd Interrupt
                        if (ds_fds[1].revents & POLLIN) {
                            ssize_t len = read(client_int, io_buf, sizeof(io_buf));
                            if (len <= 0) break;
                            write(vdsd_int, io_buf, len);
                        }
                        // vdsd Control -> Controller Control
                        if (ds_fds[2].revents & POLLIN) {
                            ssize_t len = read(vdsd_ctrl, io_buf, sizeof(io_buf));
                            if (len <= 0) break;
                            write(client_ctrl, io_buf, len);
                        }
                        // vdsd Interrupt -> Controller Interrupt (Haptik / Audio / Rumble)
                        if (ds_fds[3].revents & POLLIN) {
                            ssize_t len = read(vdsd_int, io_buf, sizeof(io_buf));
                            if (len <= 0) break;
                            write(client_int, io_buf, len);
                        }
                    }
                    close(vdsd_ctrl);
                    close(vdsd_int);
                    close(client_int);
                } 
                
                /* ========================================================
                   FALL 2: DUALSHOCK 4 - UHID TUNNELUNG (UNVERÄNDERT STABIL)
                   ======================================================== */
                else {
                    struct pollfd int_check;
                    int_check.fd = server_int;
                    int_check.events = POLLIN;
                    int client_int = -1;
                    
                    if (poll(&int_check, 1, 1500) > 0 && (int_check.revents & POLLIN)) {
                        client_int = accept(server_int, (struct sockaddr *)&client_addr, &addr_len);
                    }

                    if (client_int < 0) {
                        close(client_ctrl);
                        continue;
                    }
                    
                    set_nonblocking(client_int);
                    int uhid_fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
                    if (uhid_fd >= 0) {
                        set_nonblocking(uhid_fd);
                        
                        struct uhid_event ev = {0};
                        ev.type = UHID_CREATE;
                        strcpy((char *)ev.u.create.name, "Wireless Controller");
                        ev.u.create.bus = 0x05;
                        ev.u.create.vendor = 0x054c;
                        ev.u.create.product = 0x09cc;
                        ev.u.create.rd_size = sizeof(ds4_bt_report_desc);
                        memcpy(ev.u.create.rd_data, ds4_bt_report_desc, sizeof(ds4_bt_report_desc));
                        write(uhid_fd, &ev, sizeof(ev));

                        struct pollfd tunnel_fds[3];
                        tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN;
                        tunnel_fds[1].fd = client_int;  tunnel_fds[1].events = POLLIN;
                        tunnel_fds[2].fd = uhid_fd;     tunnel_fds[2].events = POLLIN;

                        unsigned char io_buf[BUFFER_SIZE];
                        while (1) {
                            int t_ret = poll(tunnel_fds, 3, -1);
                            if (t_ret < 0) {
                                if (errno == EINTR) continue;
                                break;
                            }
                            if ((tunnel_fds[0].revents & (POLLHUP|POLLERR)) || (tunnel_fds[1].revents & (POLLHUP|POLLERR))) {
                                break;
                            }
                            if (tunnel_fds[1].revents & POLLIN) {
