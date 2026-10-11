#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <optional>
#include <span>

// ◼ EINBETTUNG DER ORIGINALEN PROJEKT-HEADER (Verhindert Signatur-Konflikte)
#include "vds_bt.hh"

namespace vds {

// ◼ LINKER-FIX: Die Konvertierungsfunktion MUSS innerhalb des Namespaces vds deklariert sein!
extern std::optional<std::vector<std::uint8_t>> bt_feature_to_usb_feature_reply(std::span<const std::uint8_t> data);

static void setup_abstract_un(struct sockaddr_un &addr, const char *name) {
    ::memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    ::memcpy(addr.sun_path + 1, name, ::strlen(name));
}

static UniqueFd create_ipc_listener(const char *name) {
    fprintf(stderr, "vDS-CORE: UNTERSTUETZUNG FUER ABSTRAKTE UNIX-SOCKETS AKTIV! Erstelle Pipeline: @%s\n", name);
    fflush(stderr);

    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("IPC Socket Creation Failed");
    
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    struct sockaddr_un un_addr;
    setup_abstract_un(un_addr, name);
    
    socklen_t actual_len = offsetof(struct sockaddr_un, sun_path) + 1 + ::strlen(name);
    
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&un_addr), actual_len) < 0) {
        fprintf(stderr, "vDS-CORE: FATAL - Bind fuer @%s failed: %s\n", name, ::strerror(errno));
        fflush(stderr);
        ::close(fd);
        throw std::runtime_error("IPC Bind Failed");
    }
    
    if (::listen(fd, 5) < 0) {
        ::close(fd);
        throw std::runtime_error("IPC Listen Failed");
    }
    return UniqueFd(fd);
}

static std::string extract_dynamic_mac_from_payload(int client_fd) {
    std::uint8_t mac_bytes[6] = {0};
    
    int flags = ::fcntl(client_fd, F_GETFL, 0);
    ::fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
    
    ssize_t n = ::recv(client_fd, mac_bytes, 6, MSG_WAITALL | MSG_NOSIGNAL);
    
    ::fcntl(client_fd, F_SETFL, flags);

    if (n != 6) {
        fprintf(stderr, "vDS-CORE: ERROR - Proxy hat das Payload-Namensschema verletzt!\n");
        fflush(stderr);
        return "00:00:00:00:00:00";
    }

    std::stringstream ss;
    for (int i = 5; i >= 0; --i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << std::uppercase << static_cast<int>(mac_bytes[i]);
        if (i > 0) ss << ":";
    }
    return ss.str();
}

// ◼ NAMESPACE-FIX: Direkte Implementierung der im Header deklarierten Methoden, 
// ohne die Klassenstrukturen im cc-File künstlich zu duplizieren.

BtL2capAcceptor::BtL2capAcceptor() 
    : control_listener_fd_(create_ipc_listener("v_c").release()), 
      interrupt_listener_fd_(create_ipc_listener("v_i").release()) {}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_control() {
    struct sockaddr_un peer;
    socklen_t len = sizeof(struct sockaddr_un);
    ::memset(&peer, 0, sizeof(struct sockaddr_un));

    int fd = ::accept4(control_listener_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return std::nullopt;
    
    std::string mac = extract_dynamic_mac_from_payload(fd);
    fprintf(stderr, "vDS-CORE: Control-Kanal erfolgreich extrahiert. Controller-MAC: %s\n", mac.c_str());
    fflush(stderr);
    
    return BtAcceptedChannel{.address = mac, .fd = UniqueFd(fd)};
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_interrupt() {
    struct sockaddr_un peer;
    socklen_t len = sizeof(struct sockaddr_un);
    ::memset(&peer, 0, sizeof(struct sockaddr_un));

    int fd = ::accept4(interrupt_listener_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return std::nullopt;
    
    std::string mac = extract_dynamic_mac_from_payload(fd);
    fprintf(stderr, "vDS-CORE: Interrupt-Kanal erfolgreich extrahiert. Controller-MAC: %s\n", mac.c_str());
    fflush(stderr);
    
    return BtAcceptedChannel{.address = mac, .fd = UniqueFd(fd)};
}

BtL2capBackend::BtL2capBackend(std::string addr, UniqueFd c, UniqueFd i) 
    : address_(std::move(addr)), control_fd_(c.release()), interrupt_fd_(i.release()) {
    ::signal(SIGPIPE, SIG_IGN);
}

BtL2capBackend::~BtL2capBackend() { 
    if(control_fd_ >= 0) ::close(control_fd_); 
    if(interrupt_fd_ >= 0) ::close(interrupt_fd_); 
}

BtL2capBackend::BtL2capBackend(BtL2capBackend &&other) noexcept 
    : address_(std::move(other.address_)), control_fd_(other.control_fd_), interrupt_fd_(other.interrupt_fd_) {
    other.control_fd_ = -1;
    other.interrupt_fd_ = -1;
}

BtL2capBackend &BtL2capBackend::operator=(BtL2capBackend &&other) noexcept {
    if (this != &other) {
        if(control_fd_ >= 0) ::close(control_fd_);
        if(interrupt_fd_ >= 0) ::close(interrupt_fd_);
        address_ = std::move(other.address_); 
        control_fd_ = other.control_fd_;    
        interrupt_fd_ = other.interrupt_fd_;
        other.control_fd_ = -1;
        other.interrupt_fd_ = -1;
    }
    return *this;
}

void BtL2capBackend::send_output_report(std::span<const std::uint8_t> r) { 
    if (interrupt_fd_ >= 0) ::send(interrupt_fd_, r.data(), r.size(), MSG_NOSIGNAL); 
}

bool BtL2capBackend::try_send_output_report(std::span<const std::uint8_t> r) { 
    if (interrupt_fd_ < 0) return false;
    return ::send(interrupt_fd_, r.data(), r.size(), MSG_NOSIGNAL) > 0; 
}

void BtL2capBackend::send_feature_get(std::uint8_t id) {
    if (control_fd_ >= 0) {
        std::uint8_t cmd[2] = {0x43, id}; 
        ::send(control_fd_, cmd, 2, MSG_NOSIGNAL);
    }
}

void BtL2capBackend::send_feature_set(std::span<const std::uint8_t> r) {
    if (control_fd_ >= 0) {
        ::send(control_fd_, r.data(), r.size(), MSG_NOSIGNAL);
    }
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_feature_report() { 
    if (control_fd_ < 0) return std::nullopt;

    std::vector<std::uint8_t> rx_buffer(1024);
    ssize_t n = ::recv(control_fd_, rx_buffer.data(), rx_buffer.size(), MSG_DONTWAIT | MSG_NOSIGNAL);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::nullopt;
        }
        return std::nullopt; 
    }

    if (n == 0) {
        std::uint8_t peek_dummy;
        ssize_t peek_n = ::recv(control_fd_, &peek_dummy, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
        if (peek_n == 0) {
            fprintf(stderr, "vDS-CORE: Control-Pipeline physisch geschlossen (EOF).\n");
            fflush(stderr);
        }
        return std::nullopt; 
    }

    rx_buffer.resize(n);

    // ◼ NAMESPACE-MATCHNIG: Aufruf ohne globalen Scope :: da die Funktion im selben Namespace liegt
    if (rx_buffer.size() > 1 && rx_buffer[0] == 0xA3) {
        return bt_feature_to_usb_feature_reply(std::span<const std::uint8_t>(rx_buffer.data() + 1, rx_buffer.size() - 1));
    }
    return bt_feature_to_usb_feature_reply(std::span<const std::uint8_t>(rx_buffer.data(), rx_buffer.size()));
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_interrupt_packet() {
    if (interrupt_fd_ < 0) return std::nullopt;

    std::vector<std::uint8_t> buf(1024);
    ssize_t n = ::recv(interrupt_fd_, buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return std::nullopt;
        return std::nullopt;
    }
    if (n == 0) {
        std::uint8_t peek_dummy;
        ssize_t peek_n = ::recv(interrupt_fd_, &peek_dummy, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
        if (peek_n == 0) {
            fprintf(stderr, "vDS-CORE: Interrupt-Pipeline physisch geschlossen (EOF).\n");
            fflush(stderr);
        } 
        return std::nullopt; 
    }
    buf.resize(n);
    return buf;
}

} // namespace vds
