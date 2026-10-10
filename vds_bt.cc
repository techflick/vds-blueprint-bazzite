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
#include <csignal>
#include <sstream>
#include <iomanip>

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
    
    // Invariante: Bei bind() des Listeners gilt exakt offsetof + 4 Bytes (1 Byte \0 + 3 Bytes Name)
    socklen_t actual_len = offsetof(struct sockaddr_un, sun_path) + 4;
    
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

// INVARIANTE: Extrahiert die MAC-Adresse nun aus dem Payload-Header des Client-Descriptors
static std::string extract_dynamic_mac_from_payload(int client_fd) {
    std::uint8_t mac_bytes[6] = {0};
    
    // Da der Proxy die MAC synchron sendet, bevor er auf NONBLOCK schaltet,
    // lesen wir hier exakt 6 Bytes blockierend aus dem Stream.
    // Dazu temporaer NONBLOCK fuer diesen Lese-Schritt deaktivieren.
    int flags = ::fcntl(client_fd, F_GETFL, 0);
    ::fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
    
    ssize_t n = ::recv(client_fd, mac_bytes, 6, MSG_WAITALL | MSG_NOSIGNAL);
    
    // NONBLOCK-Zustand sofort wiederherstellen
    ::fcntl(client_fd, F_SETFL, flags);

    if (n != 6) {
        fprintf(stderr, "vDS-CORE: ERROR - Proxy hat das Payload-Namensschema verletzt (Keine 6 Bytes MAC)!\n");
        fflush(stderr);
        return "00:00:00:00:00:00";
    }

    std::stringstream ss;
    for (size_t i = 0; i < 6; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << std::uppercase << static_cast<int>(mac_bytes[i]);
        if (i < 5) ss << ":";
    }
    return ss.str();
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
    
    std::string mac = extract_dynamic_mac_from_payload(fd);
    fprintf(stderr, "vDS-CORE: Control-Kanal erfolgreich extrahiert. Controller-MAC: %s\n", mac.c_str());
    fflush(stderr);
    
    return BtAcceptedChannel{.address = mac, .fd = UniqueFd(fd)};
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
    
    std::string mac = extract_dynamic_mac_from_payload(fd);
    fprintf(stderr, "vDS-CORE: Interrupt-Kanal erfolgreich extrahiert. Controller-MAC: %s\n", mac.c_str());
    fflush(stderr);
    
    return BtAcceptedChannel{.address = mac, .fd = UniqueFd(fd)};
}

BtL2capBackend::BtL2capBackend(std::string addr, UniqueFd c, UniqueFd i) 
    : address_(addr), control_fd_(c.release()), interrupt_fd_(i.release()) {
    std::signal(SIGPIPE, SIG_IGN);
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
        
        // Type-Safety Invariante: Variablen-Member strikt von Gettern trennen
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
    fprintf(stderr, "vDS-SPOOF: Modalias usb:v054Cp0CE6d0100 aktiv an L2CAP gemeldet.\n");
    fflush(stderr);
}

void BtL2capBackend::send_feature_set(std::span<const std::uint8_t> r) {
    fflush(stderr);
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_feature_report() { 
    if (control_fd_ < 0) return std::nullopt;

    // INVARIANTE: Synchron/blockierend auf das Handshake-Paket warten (Kein MSG_DONTWAIT)
    std::vector<std::uint8_t> rx_buffer(65);
    ssize_t n = ::recv(control_fd_, rx_buffer.data(), rx_buffer.size(), MSG_NOSIGNAL);
    if (n <= 0) {
        fprintf(stderr, "vDS-CORE: Handshake-Anfrage auf Control-Kanal fehlgeschlagen oder geschlossen.\n");
        fflush(stderr);
        return std::nullopt;
    }

    std::vector<std::uint8_t> fake_report(65, 0x00);
    
    // HID-Header-Shift Alignment
    fake_report[0] = 0xA3; // DATA | FEATURE
    fake_report[1] = 0x05; // Report ID rueckt auf Byte 1
    
    // MAC-Adresse parsen (Format: XX:XX:XX:XX:XX:XX)
    std::uint8_t mac_bytes[6] = {0};
    std::stringstream ss(address_);
    std::string byte_str;
    int idx = 0;
    while (std::getline(ss, byte_str, ':') && idx < 6) {
        mac_bytes[idx++] = static_cast<std::uint8_t>(std::stoul(byte_str, nullptr, 16));
    }

    // INVARIANTE: Durch den Header-Shift verschiebt sich das Little-Endian-Spiegelraster strikt auf Indizes 5 bis 10
    fake_report[5] = mac_bytes[5];
    fake_report[6] = mac_bytes[4];
    fake_report[7] = mac_bytes[3];
    fake_report[8] = mac_bytes[2];
    fake_report[9] = mac_bytes[1];
    fake_report[10] = mac_bytes[0];
    
    // Restliche native Controller Firmware & Vendor-Flags (um exakt 1 Byte verschoben)
    fake_report[11] = 0x05; 
    fake_report[12] = 0xE6;
    fake_report[13] = 0x0C;
    fake_report[14] = 0x01;
    fake_report[15] = 0x00;
    fake_report[16] = 0x24; 
    
    // Unbestechliches fflush nach Setup erzwingen
    fflush(stderr);
    return fake_report;
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_interrupt_packet() {
    std::vector<std::uint8_t> buf(110);
    
    ssize_t n = ::recv(interrupt_fd_, buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        return std::nullopt;
    }
    
    if (n == 0) {
        // INVARIANTE: Symmetrischer Zero-Length & PEEK-Schutz gegen Pipeline Race-Conditions
        std::uint8_t peek_dummy;
        ssize_t peek_n = ::recv(interrupt_fd_, &peek_dummy, 1, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
        if (peek_n == 0) {
            fprintf(stderr, "vDS-CORE: Physisches EOF auf Interrupt-Kanal erkannt.\n");
            fflush(stderr);
            return std::nullopt; 
        }
        // Offene, aber leere asynchrone Iteration ueberspringen ohne zu schliessen
        return std::nullopt; 
    }
    
    buf.resize(n);
    return buf;
}

} // namespace vds
