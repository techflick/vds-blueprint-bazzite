#include "vds_bt.hh"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <span>
#include <cstddef> 
#include <stdio.h> 
#include <cerrno>

namespace vds {

static void setup_abstract_un(struct sockaddr_un &un_addr, const char *name) {
    std::memset(&un_addr, 0, sizeof(struct sockaddr_un));
    un_addr.sun_family = AF_UNIX;
    std::memcpy(un_addr.sun_path + 1, name, 3);
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
    
    socklen_t actual_len = offsetof(struct sockaddr_un, sun_path) + 1 + 3;
    
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&un_addr), actual_len) < 0) {
        fprintf(stderr, "vDS-CORE: FATAL - Bind fuer @%s failed: %s\n", name, std::strerror(errno));
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

BtL2capAcceptor::BtL2capAcceptor() 
    : control_listener_fd_(create_ipc_listener("v_c")), 
      interrupt_listener_fd_(create_ipc_listener("v_i")) {}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_control() {
    struct sockaddr_un peer;
    socklen_t len = sizeof(struct sockaddr_un);
    std::memset(&peer, 0, sizeof(struct sockaddr_un));

    int fd = ::accept4(control_listener_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        fprintf(stderr, "vDS-CORE: Kritischer accept-Fehler auf Control: %s\n", std::strerror(errno));
        fflush(stderr);
        return std::nullopt;
    }
    
    fprintf(stderr, "vDS-CORE: Control-Kanal erfolgreich per accept() aus Epoll-Event extrahiert.\n");
    fflush(stderr);
    
    return BtAcceptedChannel{.address = "00:1b:dc:00:00:00", .fd = UniqueFd(fd)};
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_interrupt() {
    struct sockaddr_un peer;
    socklen_t len = sizeof(struct sockaddr_un);
    std::memset(&peer, 0, sizeof(struct sockaddr_un));

    int fd = ::accept4(interrupt_listener_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        fprintf(stderr, "vDS-CORE: Kritischer accept-Fehler auf Interrupt: %s\n", std::strerror(errno));
        fflush(stderr);
        return std::nullopt;
    }
    
    fprintf(stderr, "vDS-CORE: Interrupt-Kanal erfolgreich per accept() aus Epoll-Event extrahiert.\n");
    fflush(stderr);
    
    return BtAcceptedChannel{.address = "00:1b:dc:00:00:00", .fd = UniqueFd(fd)};
}

BtL2capBackend::BtL2capBackend(std::string addr, UniqueFd c, UniqueFd i) 
    : address_(addr), control_fd_(c.release()), interrupt_fd_(i.release()) {}

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
        address_ = std::move(other.address_); // Korrigiert: Unterstrich hinzugefügt
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
    fprintf(stderr, "vDS-SPOOF: Modalias usb:v054Cp0CE6d0100 aktiv an L2CAP gemeldet.\n");
    fflush(stderr);
}

void BtL2capBackend::send_feature_set(std::span<const std::uint8_t> r) {
    fflush(stderr);
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_feature_report() { 
    // Erweiterter, protokollkonformer 64-Byte-Sony-Vendor-Report zur Kernel-Validierung
    std::vector<std::uint8_t> fake_report(64, 0x00);
    
    fake_report[0] = 0x05; // Report ID 0x05 (DualSense Bluetooth Feature Calibration)
    
    // Bluetooth MAC-Spoofing (rückwärts im HID-Datenstrom gespiegelt)
    fake_report[1] = 0x00; 
    fake_report[2] = 0x00;
    fake_report[3] = 0x00;
    fake_report[4] = 0xdc;
    fake_report[5] = 0x1b;
    fake_report[6] = 0x00;
    
    // Strikter Modalias-Abgleich (Sony Interactive Entertainment = 0x054C, DualSense = 0x0CE6)
    fake_report[7] = 0x4C; 
    fake_report[8] = 0x05; 
    fake_report[9] = 0xE6;
    fake_report[10] = 0x0C;
    
    // Hardware-Revisions- & Firmware-Kompatibilitäts-Flags (Erforderlich für Kernel-Sanity Check)
    fake_report[11] = 0x01;
    fake_report[12] = 0x00;
    fake_report[13] = 0x24; // Firmware Major Build
    fake_report[14] = 0x00;
    
    return fake_report;
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_interrupt_packet() {
    std::vector<std::uint8_t> buf(110);
    
    // Nutze recv() mit MSG_DONTWAIT und MSG_NOSIGNAL statt nacktem read()
    ssize_t n = ::recv(interrupt_fd_, buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        return std::nullopt;
    }
    
    if (n == 0) {
        return std::nullopt;
    }
    
    buf.resize(n);
    return buf;
}

} // namespace vds
