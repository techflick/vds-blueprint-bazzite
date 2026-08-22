#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/uhid.h>

/* Schutz vor Doppeldefinitionen aus dem System-Socket-Header */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#define BTPROTO_L2CAP 0
#define BUFFER_SIZE 1024

/* Diese Struktur entspricht exakt deinem isolierten Header aus Block 3 */
struct sockaddr_l2_local {
    uint16_t l2_family;
    uint16_t l2_psm;
    uint8_t  l2_bdaddr[6];
    uint16_t l2_cid;
    uint8_t  l2_bdaddr_type;
};

/* Valider DS4 Bluetooth-Report-Descriptor fuer das native hid-sony Modul via UHID */
static unsigned char ds4_bt_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0
};

int create_l2cap_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    /* Verhindert Port-Blockaden beim schnellen Aus- und Einschalten */
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_l2_local addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    listen(sock, 5);
    return sock;
}

int main() {
    int server_ctrl = create_l2cap_socket(0x11); /* PSM_CONTROL */
    int server_int = create_l2cap_socket(0x13);  /* PSM_INTERRUPT */
    
    if (server_ctrl < 0 || server_int < 0) return 1;

    while (1) {
        struct sockaddr_l2_local client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_ctrl = accept(server_ctrl, (struct sockaddr *)&client_addr, &addr_len);
        
        if (client_ctrl >= 0) {
            char peek_buf[BUFFER_SIZE] = {0};
            /* MSG_PEEK liest den Paketkopf, ohne die Kernel-Warteschlange zu leeren */
            ssize_t peek_len = recv(client_ctrl, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);

            /* PRIORITAET 1: DUALSENSE PRUEFUNG */
            if (peek_len > 0 && (strstr(peek_buf, "DualSense") != NULL || peek_buf == 0x01)) {
                /* Ein DualSense wurde erkannt! */
                /* Wir schliessen den Proxy-Socket SOFORT und machen den Weg im Kernel frei */
                close(client_ctrl);
                
                /* Der Kernel-Treiber uebernimmt, udev aus deinem alten Block 6 feuert wieder! */
                continue; 
            } else {
                /* PRIORITAET 2: DUALSHOCK 4 (UHID-WEICHE) */
                int client_int = accept(server_int, (struct sockaddr *)&client_addr, &addr_len);
                if (client_int >= 0) {
                    int uhid_fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
                    if (uhid_fd >= 0) {
                        struct uhid_event ev = {0};
                        ev.type = UHID_CREATE;
                        strcpy((char *)ev.u.create.name, "Wireless Controller");
                        ev.u.create.bus = 0x05; /* BUS_BLUETOOTH */
                        ev.u.create.vendor = 0x054c;
                        ev.u.create.product = 0x09cc; /* DS4 Slim/Pro */
                        ev.u.create.rd_size = sizeof(ds4_bt_report_desc);
                        memcpy(ev.u.create.rd_data, ds4_bt_report_desc, sizeof(ds4_bt_report_desc));
                        write(uhid_fd, &ev, sizeof(ev));

                        /* Kontinuierliche Datenspiegelung fuer den DS4 in den UHID-Tunnel */
                        unsigned char io_buf[BUFFER_SIZE];
                        while (1) {
                            ssize_t len = read(client_int, io_buf, sizeof(io_buf));
                            if (len <= 0) break;
                            
                            struct uhid_event in_ev = {0};
                            in_ev.type = UHID_INPUT;
                            in_ev.u.input.size = len;
                            memcpy(in_ev.u.input.data, io_buf, len);
                            write(uhid_fd, &in_ev, sizeof(in_ev));
                        }
                        close(uhid_fd);
                    }
                    close(client_int);
                }
                close(client_ctrl);
            }
        }
    }
    return 0;
}
