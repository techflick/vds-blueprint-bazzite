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

// Nutzt deine generierten, verifizierten Header aus Block 3
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#define BUFFER_SIZE 1024

// Da BTPROTO_L2CAP im Block-3-Header 0 ist, fixieren wir hier den echten Kernel-Wert 3
#define REAL_BTPROTO_L2CAP 3

static unsigned char ds4_bt_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int create_l2cap_socket(uint16_t psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, REAL_BTPROTO_L2CAP);
    if (sock < 0) return -1;
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_l2 addr;
    memset(&addr, 0, sizeof(struct sockaddr_l2));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(psm);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_l2)) < 0) {
        close(sock);
        return -1;
    }
    listen(sock, 5);
    set_nonblocking(sock);
    return sock;
}

int connect_to_vdsd(uint16_t target_psm) {
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, REAL_BTPROTO_L2CAP);
    if (sock < 0) return -1;

    struct sockaddr_l2 addr;
    memset(&addr, 0, sizeof(struct sockaddr_l2));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(target_psm);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_l2)) < 0) {
        close(sock);
        return -1;
    }
    set_nonblocking(sock);
    return sock;
}
int main() {
    int server_ctrl = create_l2cap_socket(0x11);
    int server_int = create_l2cap_socket(0x13);
    
    if (server_ctrl < 0 || server_int < 0) {
        fprintf(stderr, "[-] Initialisierung der L2CAP-Server-Sockets fehlgeschlagen.\n");
        return 1;
    }

    struct pollfd s_fds[2];
    s_fds[0].fd = server_ctrl;  s_fds[0].events = POLLIN;
    s_fds[1].fd = server_int;   s_fds[1].events = POLLIN;

    printf("[+] vDS Dynamic Bluetooth Socket Router aktiv. Warte auf Controller...\n");

    while (1) {
        int ret = poll(s_fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < 2; i++) {
            if (s_fds[i].revents & POLLIN) {
                struct sockaddr_storage client_addr;
                socklen_t addr_len = sizeof(struct sockaddr_storage);
                int client_ctrl = accept(s_fds[i].fd, (struct sockaddr *)&client_addr, &addr_len);
                
                if (client_ctrl >= 0) {
                    set_nonblocking(client_ctrl);
                    char peek_buf[BUFFER_SIZE];
                    memset(peek_buf, 0, sizeof(peek_buf));
                    
                    usleep(15000); 
                    ssize_t peek_len = recv(client_ctrl, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK | MSG_DONTWAIT);

                    int is_dualsense = 0;
                    if (peek_len > 0) {
                        if (memmem(peek_buf, peek_len, "DualSense", 9) != NULL || peek_buf[0] == 0x01) {
                            is_dualsense = 1;
                        }
                    }

                    if (is_dualsense) {
                        printf("[+] DualSense erkannt. Initialisiere Weiche zu vdsd...\n");
                        
                        struct pollfd int_check;
                        int_check.fd = server_int;
                        int_check.events = POLLIN;
                        int client_int = -1;
                        
                        if (poll(&int_check, 1, 1500) > 0 && (int_check.revents & POLLIN)) {
                            addr_len = sizeof(struct sockaddr_storage);
                            client_int = accept(server_int, (struct sockaddr *)&client_addr, &addr_len);
                        }

                        if (client_int < 0) {
                            close(client_ctrl);
                            continue;
                        }
                        set_nonblocking(client_int);

                        int vdsd_ctrl = connect_to_vdsd(0x21);
                        int vdsd_int = connect_to_vdsd(0x23);

                        if (vdsd_ctrl < 0 || vdsd_int < 0) {
                            printf("[-] Verbindung zu vdsd fehlgeschlagen.\n");
                            close(client_ctrl); close(client_int);
                            if (vdsd_ctrl >= 0) close(vdsd_ctrl);
                            if (vdsd_int >= 0) close(vdsd_int);
                            continue;
                        }

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
                            for (int k = 0; k < 4; k++) {
                                if (ds_fds[k].revents & (POLLHUP | POLLERR)) active = 0;
                            }
                            if (!active) break;

                            if (ds_fds[0].revents & POLLIN) {
                                ssize_t len = read(client_ctrl, io_buf, sizeof(io_buf));
                                if (len <= 0) break;
                                if (write(vdsd_ctrl, io_buf, len) < 0) break;
                            }
                            if (ds_fds[1].revents & POLLIN) {
                                ssize_t len = read(client_int, io_buf, sizeof(io_buf));
                                if (len <= 0) break;
                                if (write(vdsd_int, io_buf, len) < 0) break;
                            }
                            if (ds_fds[2].revents & POLLIN) {
                                ssize_t len = read(vdsd_ctrl, io_buf, sizeof(io_buf));
                                if (len <= 0) break;
                                if (write(client_ctrl, io_buf, len) < 0) break;
                            }
                            if (ds_fds[3].revents & POLLIN) {
                                ssize_t len = read(vdsd_int, io_buf, sizeof(io_buf));
                                if (len <= 0) break;
                                if (write(client_int, io_buf, len) < 0) break;
                            }
                        }
                        close(vdsd_ctrl); close(vdsd_int); close(client_int);
                    }
                    else {
                        printf("[+] Alternativer Controller erkannt. Starte UHID-Tunnelung...\n");
                        
                        struct pollfd int_check;
                        int_check.fd = server_int;
                        int_check.events = POLLIN;
                        int client_int = -1;
                        
                        if (poll(&int_check, 1, 1500) > 0 && (int_check.revents & POLLIN)) {
                            addr_len = sizeof(struct sockaddr_storage);
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
                            
                            struct uhid_event *ev = calloc(1, sizeof(struct uhid_event));
                            if (ev != NULL) {
                                ev->type = UHID_CREATE;
                                strncpy((char *)ev->u.create.name, "Wireless Controller", sizeof(ev->u.create.name) - 1);
                                ev->u.create.bus = 0x05; 
                                ev->u.create.vendor = 0x054c;
                                ev->u.create.product = 0x09cc; 
                                ev->u.create.rd_size = sizeof(ds4_bt_report_desc);
                                memcpy(ev->u.create.rd_data, ds4_bt_report_desc, sizeof(ds4_bt_report_desc));
                                
                                if (write(uhid_fd, ev, sizeof(struct uhid_event)) >= 0) {
                                    struct pollfd tunnel_fds[3];
                                    tunnel_fds[0].fd = client_ctrl; tunnel_fds[0].events = POLLIN;
                                    tunnel_fds[1].fd = client_int;  tunnel_fds[1].events = POLLIN;
                                    tunnel_fds[2].fd = uhid_fd;     tunnel_fds[2].events = POLLIN;

                                    unsigned char io_buf[BUFFER_SIZE];
                                    int tunnel_active = 1;

                                    struct uhid_event *in_ev = calloc(1, sizeof(struct uhid_event));
                                    struct uhid_event *out_ev = calloc(1, sizeof(struct uhid_event));

                                    if (in_ev != NULL && out_ev != NULL) {
                                        while (tunnel_active) {
                                            int t_ret = poll(tunnel_fds, 3, -1);
                                            if (t_ret < 0) {
                                                if (errno == EINTR) continue;
                                                break;
                                            }
                                            
                                            if ((tunnel_fds[0].revents & (POLLHUP|POLLERR)) || 
                                                (tunnel_fds[1].revents & (POLLHUP|POLLERR)) ||
                                                (tunnel_fds[2].revents & (POLLHUP|POLLERR))) {
                                                break;
                                            }

                                            if (tunnel_fds[1].revents & POLLIN) {
                                                ssize_t len = read(client_int, io_buf, sizeof(io_buf));
                                                if (len <= 0) break;
                                                
                                                memset(in_ev, 0, sizeof(struct uhid_event));
                                                in_ev->type = UHID_INPUT2; 
                                                in_ev->u.input2.size = len;
                                                memcpy(in_ev->u.input2.data, io_buf, len);
                                                
                                                size_t write_len = sizeof(in_ev->type) + sizeof(in_ev->u.input2.size) + len;
                                                if (write(uhid_fd, in_ev, write_len) < 0) break;
                                            }

                                            if (tunnel_fds[0].revents & POLLIN) {
                                                ssize_t len = read(client_ctrl, io_buf, sizeof(io_buf));
                                                if (len <= 0) break;
                                            }

                                            if (tunnel_fds[2].revents & POLLIN) {
                                                memset(out_ev, 0, sizeof(struct uhid_event));
                                                ssize_t u_len = read(uhid_fd, out_ev, sizeof(struct uhid_event));
                                                if (u_len > 0) {
                                                    if (out_ev->type == UHID_OUTPUT) {
                                                        if (write(client_int, out_ev->u.output.data, out_ev->u.output.size) < 0) break;
                                                    }
                                                } else if (u_len < 0 && errno != EAGAIN) {
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    if (in_ev) free(in_ev);
                                    if (out_ev) free(out_ev);
                                }
                                free(ev);
                            }
                            close(uhid_fd);
                        }
                        close(client_int);
                    }
                    close(client_ctrl);
                }
            }
        }
    }
    return 0;
}
