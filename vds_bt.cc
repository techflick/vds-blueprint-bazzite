#include "vds_bt.hh"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <span>
#include <cstddef> // Erforderlich für offsetof

namespace vds {

static void setup_abstract_un(struct sockaddr_un &un_addr, const char *name) {
    std::memset(&un_addr, 0, sizeof(struct sockaddr_un));
    un_addr.sun_family = AF_UNIX;
    
    // Das erste Byte (un_addr.sun_path[0]) bleibt \0 für den abstrakten Namespace.
    // Kopiere exakt die 3 Zeichen ("v_c" oder "v_i") ab Position sun_path + 1.
    std::memcpy(un_addr.sun_path + 1, name, 3);
}

static UniqueFd create_ipc_listener(const char *name) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) throw std::runtime_error("IPC Socket Creation Failed");
    
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    struct sockaddr_un un_addr;
    setup_abstract_un(un_addr, name);
    
    // KORREKTUR: Berechne die exakte Bytegröße des deklarierten Namens im RAM.
    // 2 Bytes (sun_family) + 1 Byte (\0) + 3 Bytes (Name) = Exakt 6 Bytes.
    socklen_t actual_len = offsetof(struct sockaddr_un, sun_path) + 1 + 3;
    
    // Dem Kernel wird NUR die tatsächliche Länge übergeben.
    // Das verhindert das Auffüllen des Schlüssels mit den restlichen 106 Nullbytes.
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&un_addr), actual_len) < 0) {
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

    int fd = ::accept(control_listener_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len);
    if (fd < 0) return std::nullopt;
    
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return BtAcceptedChannel{.address = "00:1b:dc:00:00:00", .fd = UniqueFd(fd)};
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_interrupt() {
    struct sockaddr_un peer;
    socklen_t len = sizeof(struct sockaddr_un);
    std::memset(&peer, 0, sizeof(struct sockaddr_un));

    int fd = ::accept(interrupt_listener_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len);
    if (fd < 0) return std::nullopt;
    
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return BtAcceptedChannel{.address = "00:1b:dc:00:00:00", .fd = UniqueFd(fd)};
}

// Backend-Methoden zur Absicherung der ABI-Stabilität
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
        address_ = std::move(other.address_);
        control_fd_ = other.control_fd_;
        interrupt_fd_ = other.interrupt_fd_;
        other.control_fd_ = -1;
        other.interrupt_fd_ = -1;
    }
    return *this;
}

void BtL2capBackend::send_output_report(std::span<const std::uint8_t> r) { ::write(interrupt_fd_, r.data(), r.size()); }
bool BtL2capBackend::try_send_output_report(std::span<const std::uint8_t> r) { return ::write(interrupt_fd_, r.data(), r.size()) > 0; }
void BtL2capBackend::send_feature_get(std::uint8_t id) {}
void BtL2capBackend::send_feature_set(std::span<const std::uint8_t> r) {}
std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_feature_report() { return std::nullopt; }

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_interrupt_packet() {
    std::vector<std::uint8_t> buf(110);
    int n = ::read(interrupt_fd_, buf.data(), buf.size());
    if(n <= 0) return std::nullopt;
    buf.resize(n);
    return buf;
}

} // namespace vds
