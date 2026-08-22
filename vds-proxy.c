#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <linux/uhid.h>

// Dynamische Bluetooth-Strukturen (ohne externe BlueZ-Abhängigkeit)
#define PSM_CTRL 0x11
#define PSM_INTR 0x13
#define AF_BLUETOOTH 31

typedef struct { unsigned char b[6]; } __attribute__((packed)) bdaddr_t;
struct sockaddr_l2 {
    unsigned short l2_family;
    unsigned short l2_psm;
    bdaddr_t l2_bdaddr;
    unsigned short l2_cid;
    unsigned char l2_bdaddr_type;
};

// Originaler HID Report Descriptor für den DualShock 4 über Bluetooth (für hid-sony)
static unsigned char ds4_bt_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25,
    0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x01, 0x06, 0x00, 0xff, 0x09, 0x20, 0x75, 0x08, 0x95, 0x05, 0x81,
    0x02, 0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x02, 0x06, 0x00, 0xff, 0x09, 0x21, 0x75, 0x08, 0x95, 0x36, 0x81, 0x02, 0x85, 0x11, 0x09,
    0x22, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x12, 0x09, 0x23, 0x75, 0x08, 0x95, 0x4d, 0x91,
    0x02, 0x85, 0x13, 0x09, 0x24, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x14, 0x09, 0x25, 0x75,
    0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x15, 0x09, 0x26, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85,
    0x16, 0x09, 0x27, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x17, 0x09, 0x28, 0x75, 0x08, 0x95,
    0x4d, 0x91, 0x02, 0x85, 0x18, 0x09, 0x29, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x19, 0x09,
    0x2a, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x1a, 0x09, 0x2b, 0x75, 0x08, 0x95, 0x4d, 0x91,
    0x02, 0x85, 0x1b, 0x09, 0x2c, 0x75, 0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0x1c, 0x09, 0x2d, 0x75,
    0x08, 0x95, 0x4d, 0x91, 0x02, 0x85, 0xa0, 0x09, 0x2e, 0x75, 0x08, 0x95, 0x07, 0x81, 0x02, 0x85,
    0xb0, 0x09, 0x2f, 0x75, 0x08, 0x95, 0x02, 0xb1, 0x02, 0xc0
};

static void trigger_vds_daemon_attach(const char *mac) {
    char command[256];
    snprintf(command, sizeof(command), "/usr/bin/vdsctl attach %s --profile ds5 --ports 0 >/dev/null 2>&1 &", mac);
    system(command);
}

int create_uhid_device(bdaddr_t *bdaddr) {
    int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE;
    
    strcpy((char*)ev.u.create.name, "Wireless Controller");
    ev.u.create.bus = 0x05; 
    ev.u.create.vendor = 0x054c;   
    ev.u.create.product = 0x05c4;  
    
    sprintf((char*)ev.u.create.phys, "%02X:%02X:%02X:%02X:%02X:%02X",
            bdaddr->b[5], bdaddr->b[4], bdaddr->b[3], bdaddr->b[2], bdaddr->b[1], bdaddr->b[0]);

    ev.u.create.rd_data = ds4_bt_report_desc;
    ev.u.create.rd_size = sizeof(ds4_bt_report_desc);

    if (write(fd, &ev, sizeof(ev)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main() {
    struct sockaddr_l2 addr;
    socklen_t opt = sizeof(addr);
    
    int ctrl_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, 0);
    int intr_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, 0);
    
    int reuse = 1;
    setsockopt(ctrl_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(intr_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    
    addr.l2_psm = PSM_CTRL;
    bind(ctrl_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctrl_sock, 10);
    
    addr.l2_psm = PSM_INTR;
    bind(intr_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(intr_sock, 10);

    struct pollfd server_fds[2];
    server_fds[0].fd = ctrl_sock; server_fds[0].events = POLLIN;
    server_fds[1].fd = intr_sock; server_fds[1].events = POLLIN;

    printf("vDS-Bypass-Proxy aktiv (Lausche auf PSM 0x11/0x13)...\n");

    while (1) {
        if (poll(server_fds, 2, -1) < 0) continue;

        if (server_fds[0].revents & POLLIN) {
            int client_ctrl = accept(ctrl_sock, (struct sockaddr *)&addr, &opt);
            if (client_ctrl < 0) continue;

            int client_intr = accept(intr_sock, (struct sockaddr *)&addr, &opt);
            if (client_intr < 0) {
                close(client_ctrl);
                continue;
            }

            unsigned char peek_buf[256];
            int is_dualsense = 0;
            
            int peek_len = recv(client_ctrl, peek_buf, sizeof(peek_buf), MSG_PEEK);
            if (peek_len > 0) {
                for (int i = 0; i < peek_len - 9; i++) {
                    if (memcmp(&peek_buf[i], "DualSense", 9) == 0) {
                        is_dualsense = 1;
                        break;
                    }
                }
            }

            char mac_str[18];
            sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", 
                    addr.l2_bdaddr.b[5], addr.l2_bdaddr.b[4], addr.l2_bdaddr.b[3], 
                    addr.l2_bdaddr.b[2], addr.l2_bdaddr.b[1], addr.l2_bdaddr.b[0]);

            if (is_dualsense) {
                printf("[vDS-Proxy] DualSense erkannt (%s)! Uebergabe an vDS-Daemon.\n", mac_str);
                close(client_ctrl);
                close(client_intr);
                
                usleep(100000); 
                trigger_vds_daemon_attach(mac_str);
                continue; 
            }

            printf("[vDS-Proxy] DualShock 4 erkannt (%s). Erzeuge UHID-Tunnel...\n", mac_str);
            int uhid_fd = create_uhid_device(&addr.l2_bdaddr);
            if (uhid_fd < 0) {
                close(client_ctrl);
                close(client_intr);
                continue;
            }

            struct pollfd tunnel_fds[2];
            tunnel_fds[0].fd = client_intr; tunnel_fds[0].events = POLLIN;
            tunnel_fds[1].fd = uhid_fd;     tunnel_fds[1].events = POLLIN;

            unsigned char buf[1024];
            int running = 1;

            while (running) {
                if (poll(tunnel_fds, 2, -1) < 0) break;

                if (tunnel_fds[0].revents & POLLIN) {
                    int len = read(client_intr, buf, sizeof(buf));
                    if (len <= 0) { running = 0; break; }

                    struct uhid_event ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.type = UHID_INPUT;
                    ev.u.input.size = len;
                    memcpy(ev.u.input.data, buf, len);
                    if (write(uhid_fd, &ev, sizeof(ev)) < 0) { running = 0; break; }
                }

                if (tunnel_fds[1].revents & POLLIN) {
                    struct uhid_event ev;
                    int len = read(uhid_fd, &ev, sizeof(ev));
                    if (len > 0 && ev.type == UHID_OUTPUT) {
                        write(client_intr, ev.u.output.data, ev.u.output.size);
                    }
                }
            }

            struct uhid_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = UHID_DESTROY;
            write(uhid_fd, &ev, sizeof(ev));
            close(uhid_fd);
            close(client_ctrl);
            close(client_intr);
            printf("[vDS-Proxy] Verbindung fuer %s beendet.\n", mac_str);
        }
    }
    return 0;
}
