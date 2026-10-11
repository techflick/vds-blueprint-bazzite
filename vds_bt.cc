#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <span>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_bt.hh"
#include "vds_io.hh"
#include "vds_protocol.hh"

namespace vds {

namespace {

constexpr std::size_t kBtReadBufferSize = 1024;
constexpr int kBtWritePollTimeoutMs = 1000;

void close_if_open(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// Hilfsfunktion für das Senden über das 2-Byte-Stream-Framing mit Debugging
bool send_framed_packet(int fd, std::span<const std::uint8_t> data) {
  if (fd < 0) return false;
  std::uint16_t len_be = htons(static_cast<std::uint16_t>(data.size()));
  
  std::fprintf(stderr, "vDS-Daemon-Debug [SEND]: Versuche %zu Bytes zu senden (Framed-Header: %u)...\n", data.size(), (unsigned int)data.size());

  // 1. Sende den 2-Byte-Längen-Header (Big Endian)
  if (::send(fd, &len_be, 2, MSG_DONTWAIT | MSG_NOSIGNAL) != 2) {
    std::fprintf(stderr, "vDS-Daemon-Debug [SEND-ERR]: Header-Versand fehlgeschlagen auf fd=%d, errno=%d\n", fd, errno);
    return false;
  }
  
  // 2. Sende die eigentliche Payload
  ssize_t n = ::send(fd, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
  if (n != static_cast<ssize_t>(data.size())) {
    std::fprintf(stderr, "vDS-Daemon-Debug [SEND-ERR]: Payload-Versand unvollständig auf fd=%d (%zd/%zu Bytes), errno=%d\n", fd, n, data.size(), errno);
    return false;
  }

  std::fprintf(stderr, "vDS-Daemon-Debug [SEND-OK]: Paket erfolgreich über Stream fd=%d geschickt.\n", fd);
  return true;
}

// Hilfsfunktion für das atomare Lesen eines Stream-Pakets via MSG_PEEK mit Debugging
std::optional<std::vector<std::uint8_t>> read_framed_packet(int fd) {
  if (fd < 0) return std::nullopt;

  std::uint16_t len_be;
  // Prüfen, ob der Header da ist
  ssize_t n = ::recv(fd, &len_be, 2, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
  if (n < 2) {
    if (n == 0) {
      std::fprintf(stderr, "vDS-Daemon-Debug [RECV-DISCONN]: Verbindung vom Proxy auf fd=%d getrennt.\n", fd);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      std::fprintf(stderr, "vDS-Daemon-Debug [RECV-ERR]: Header-PEEK fehlgeschlagen auf fd=%d, errno=%d\n", fd, errno);
    }
    return std::nullopt; 
  }

  std::uint16_t packet_len = ntohs(len_be);
  std::vector<std::uint8_t> buf(2 + packet_len);

  // Prüfen, ob das gesamte Paket im Kernel-Puffer liegt
  n = ::recv(fd, buf.data(), buf.size(), MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
  if (n < static_cast<ssize_t>(buf.size())) {
    static int fragment_count = 0;
    if (fragment_count++ % 100 == 0) {
      std::fprintf(stderr, "vDS-Daemon-Debug [RECV-FRAG]: Unvollständiges Paket auf fd=%d. Erwarte %d Bytes, im Puffer sind %zd.\n", fd, 2 + packet_len, n);
    }
    return std::nullopt; 
  }

  // Paket endgültig aus der Pipeline konsumieren
  ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
  std::fprintf(stderr, "vDS-Daemon-Debug [RECV-OK]: Stream-Paket komplett konsumiert (fd=%d, Payload: %u Bytes).\n", fd, packet_len);

  return std::vector<std::uint8_t>(buf.begin() + 2, buf.end());
}

static void setup_abstract_un(struct sockaddr_un &addr, const char *name) {
  ::memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  ::memcpy(addr.sun_path + 1, name, ::strlen(name));
}

UniqueFd listen_ipc_pipe(const char *name) {
  UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw std::runtime_error("failed to open IPC Unix socket: " + std::string(std::strerror(errno)));
  }
  set_nonblocking(fd.get(), "socket");

  const int reuse = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_un sockaddr{};
  setup_abstract_un(sockaddr, name);
  socklen_t actual_len = offsetof(struct sockaddr_un, sun_path) + 1 + ::strlen(name);

  if (::bind(fd.get(), reinterpret_cast<const struct sockaddr *>(&sockaddr), actual_len) < 0) {
    throw std::runtime_error("failed to bind IPC socket @" + std::string(name) + ": " + std::strerror(errno));
  }
  if (::listen(fd.get(), 5) < 0) {
    throw std::runtime_error("failed to listen on IPC socket @" + std::string(name) + ": " + std::strerror(errno));
  }
  return fd;
}

std::optional<BtAcceptedChannel> accept_ipc_pipe(int listener_fd) {
  struct sockaddr_un peer{};
  socklen_t peer_size = sizeof(peer);
  
  std::fprintf(stderr, "vDS-Daemon-Debug [INIT]: Warte auf incoming accept() vom Proxy auf listener_fd=%d...\n", listener_fd);
  UniqueFd fd(::accept(listener_fd, reinterpret_cast<struct sockaddr *>(&peer), &peer_size));
  if (!fd) {
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
      return {};
    }
    std::fprintf(stderr, "vDS-Daemon-Debug [INIT-ERR]: accept() fehlgeschlagen, error=%d\n", error);
    throw std::runtime_error("failed to accept incoming IPC connection: " + std::string(std::strerror(error)));
  }

  std::fprintf(stderr, "vDS-Daemon-Debug [INIT]: Verbindung akzeptiert (fd=%d). Starte blockierende Identitäts-Phase (Warte auf 6-Byte MAC)...\n", fd.get());

  if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0) {
    throw std::runtime_error("failed to set close-on-exec for IPC pipe: " + std::string(std::strerror(errno)));
  }

  // Identitäts-Phase: Auslesen der 6-Byte-MAC im blockierenden Zustand
  std::uint8_t mac_bytes[6] = {0};
  ssize_t n = ::recv(fd.get(), mac_bytes, 6, MSG_WAITALL | MSG_NOSIGNAL);
  if (n != 6) {
    std::fprintf(stderr, "vDS-Daemon-Debug [INIT-ERR]: MAC-Handshake ungültig! Nur %zd von 6 Bytes erhalten. Schließe Verbindung.\n", n);
    return {}; 
  }

  // MAC in Text umwandeln
  char mac_str[18]{};
  std::snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac_bytes[0], mac_bytes[1], mac_bytes[2], mac_bytes[3], mac_bytes[4], mac_bytes[5]);

  std::fprintf(stderr, "vDS-Daemon-Debug [INIT-OK]: Handshake erfolgreich. Controller-MAC: %s. Schalte Socket auf asynchron.\n", mac_str);

  set_nonblocking(fd.get(), "socket");

  return BtAcceptedChannel{
      .address = mac_str,
      .fd = std::move(fd),
  };
}

} // namespace

BtL2capAcceptor::BtL2capAcceptor()
    : control_listener_fd_(listen_ipc_pipe("v_c")),
      interrupt_listener_fd_(listen_ipc_pipe("v_i")) {}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_control() {
  return accept_ipc_pipe(control_listener_fd_.get());
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_interrupt() {
  return accept_ipc_pipe(interrupt_listener_fd_.get());
}

BtL2capBackend::BtL2capBackend(std::string address, UniqueFd control_fd,
                               UniqueFd interrupt_fd)
    : address_(std::move(address)), control_fd_(control_fd.release()),
      interrupt_fd_(interrupt_fd.release()) {}

BtL2capBackend::~BtL2capBackend() {
  close_if_open(interrupt_fd_);
  close_if_open(control_fd_);
}

BtL2capBackend::BtL2capBackend(BtL2capBackend &&other) noexcept
    : address_(std::move(other.address_)), control_fd_(other.control_fd_),
      interrupt_fd_(other.interrupt_fd_) {
  other.control_fd_ = -1;
  other.interrupt_fd = -1;
}

BtL2capBackend &BtL2capBackend::operator=(BtL2capBackend &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  close_if_open(interrupt_fd_);
  close_if_open(control_fd_);
  address_ = std::move(other.address_);
  control_fd = other.control_fd_;
  interrupt_fd_ = other.interrupt_fd_;
  other.control_fd_ = -1;
  other.interrupt_fd_ = -1;
  return *this;
}

void BtL2capBackend::send_output_report(std::span<const std::uint8_t> report) {
  if (interrupt_fd_ < 0) return;
  const auto packet = hidp_output_packet(report);
  send_framed_packet(interrupt_fd_, packet);
}

bool BtL2capBackend::try_send_output_report(std::span<const std::uint8_t> report) {
  if (interrupt_fd_ < 0) return false;
  const auto packet = hidp_output_packet(report);
  return send_framed_packet(interrupt_fd_, packet);
}

void BtL2capBackend::send_feature_get(std::uint8_t report_id) {
  if (control_fd_ < 0) return;
  const auto packet = feature_get_packet(report_id);
  send_framed_packet(control_fd_, packet);
}

void BtL2capBackend::send_feature_set(std::span<const std::uint8_t> report) {
  if (control_fd_ < 0) return;
  const auto packet = feature_set_packet(report);
  if (!packet.empty()) {
    send_framed_packet(control_fd_, packet);
  }
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_feature_report() {
  auto packet = read_framed_packet(control_fd_);
  if (!packet) return std::nullopt;

  return bt_feature_to_usb_feature_reply(
      std::span(packet->data(), packet->size()));
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_interrupt_packet() {
  return read_framed_packet(interrupt_fd_);
}

} // namespace vds
