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
#include <csignal>

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

bool send_framed_packet(int fd, std::span<const std::uint8_t> data) {
  if (fd < 0) return false;
  std::uint16_t len_be = htons(static_cast<std::uint16_t>(data.size()));
  
  std::fprintf(stderr, "vDS-Daemon-Debug [SEND]: Versuche %zu Bytes zu senden...\n", data.size());

  if (::send(fd, &len_be, 2, MSG_DONTWAIT | MSG_NOSIGNAL) != 2) {
    std::fprintf(stderr, "vDS-Daemon-Debug [SEND-ERR]: Header-Versand fehlgeschlagen, fd=%d, errno=%d\n", fd, errno);
    return false;
  }
  
  ssize_t n = ::send(fd, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
  if (n != static_cast<ssize_t>(data.size())) {
    std::fprintf(stderr, "vDS-Daemon-Debug [SEND-ERR]: Payload unvollständig, fd=%d, errno=%d\n", fd, errno);
    return false;
  }

  return true;
}

std::optional<std::vector<std::uint8_t>> read_framed_packet(int fd) {
  if (fd < 0) return std::nullopt;

  std::uint16_t len_be;
  ssize_t n = ::recv(fd, &len_be, 2, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
  
  if (n < 2) {
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return std::nullopt;
    if (n == 0) return std::nullopt;
    std::fprintf(stderr, "vDS-Daemon-Debug [RECV-ERR]: Header-PEEK fehlgeschlagen, fd=%d, errno=%d\n", fd, errno);
    return std::nullopt; 
  }

  std::uint16_t packet_len = ntohs(len_be);
  std::vector<std::uint8_t> buf(2 + packet_len);

  n = ::recv(fd, buf.data(), buf.size(), MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
  if (n < static_cast<ssize_t>(buf.size())) return std::nullopt; 

  ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
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

std::optional<BtAcceptedChannel> accept_ipc_pipe(int listener_fd, const char *channel_name) {
  struct sockaddr_un peer{};
  socklen_t peer_size = sizeof(peer);
  
  std::fprintf(stderr, "vDS-Daemon-Debug [INIT]: Warte auf accept() auf %s...\n", channel_name);
  
  int raw_fd = ::accept4(listener_fd, reinterpret_cast<struct sockaddr *>(&peer), &peer_size, SOCK_CLOEXEC);
  if (raw_fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return std::nullopt;
    throw std::runtime_error("failed to accept incoming IPC connection: " + std::string(std::strerror(errno)));
  }

  UniqueFd fd(raw_fd);
  std::fprintf(stderr, "vDS-Daemon-Debug [INIT]: %s akzeptiert (fd=%d). Warte auf 6-Byte MAC...\n", channel_name, fd.get());

  std::uint8_t mac_bytes[6] = {0};
  ssize_t n = ::recv(fd.get(), mac_bytes, 6, MSG_WAITALL | MSG_NOSIGNAL);
  if (n != 6) {
    std::fprintf(stderr, "vDS-Daemon-Debug [INIT-ERR]: MAC unvollständig (%zd/6 Bytes), trenne fd=%d\n", n, fd.get());
    return std::nullopt; 
  }

  set_nonblocking(fd.get(), channel_name);

  char mac_str[18]{};
  std::snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac_bytes[0], mac_bytes[1], mac_bytes[2], mac_bytes[3], mac_bytes[4], mac_bytes[5]);

  return BtAcceptedChannel{ .address = mac_str, .fd = std::move(fd) };
}

UniqueFd accept_interrupt_pipe_pure(int listener_fd, const char *channel_name) {
  struct sockaddr_un peer{};
  socklen_t peer_size = sizeof(peer);
  
  int raw_fd = ::accept4(listener_fd, reinterpret_cast<struct sockaddr *>(&peer), &peer_size, SOCK_CLOEXEC);
  if (raw_fd < 0) {
    throw std::runtime_error("failed to accept interrupt IPC connection: " + std::string(std::strerror(errno)));
  }

  UniqueFd fd(raw_fd);
  set_nonblocking(fd.get(), channel_name);
  std::fprintf(stderr, "vDS-Daemon-Debug [INIT]: %s (Interrupt-Kanal) rein pass-through aktiv (fd=%d).\n", channel_name, fd.get());
  return fd;
}
} // namespace (anonym)

BtL2capAcceptor::BtL2capAcceptor()
    : control_listener_fd_(listen_ipc_pipe("v_c")),
      interrupt_listener_fd_(listen_ipc_pipe("v_i")) {
  ::signal(SIGPIPE, SIG_IGN);
  std::fflush(stderr);
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_control() {
  return accept_ipc_pipe(control_listener_fd_.get(), "vdsd_ctrl");
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_interrupt() {
  return std::nullopt;
}

std::optional<BtL2capBackend> BtL2capAcceptor::establish_backend_blocking() {
  auto control_channel = accept_control();
  if (!control_channel) return std::nullopt;

  UniqueFd interrupt_fd;
  try {
    interrupt_fd = accept_interrupt_pipe_pure(interrupt_listener_fd_.get(), "vdsd_intr");
  } catch (const std::exception &e) {
    std::fprintf(stderr, "vDS-Daemon-Debug [ERR]: exception in accept_interrupt_pipe_pure: %s\n", e.what());
    return std::nullopt;
  }

  return BtL2capBackend(control_channel->address, std::move(control_channel->fd), std::move(interrupt_fd));
}

BtL2capBackend::BtL2capBackend(std::string address, UniqueFd control_fd, UniqueFd interrupt_fd)
    : address_(std::move(address)), control_fd_(control_fd.release()), interrupt_fd_(interrupt_fd.release()) {}

BtL2capBackend::~BtL2capBackend() {
  close_if_open(interrupt_fd_);
  close_if_open(control_fd_);
}

BtL2capBackend::BtL2capBackend(BtL2capBackend &&other) noexcept
    : address_(std::move(other.address_)), control_fd_(other.control_fd_), interrupt_fd_(other.interrupt_fd_) {
  other.control_fd = -1; other.interrupt_fd_ = -1; 
}

BtL2capBackend &BtL2capBackend::operator=(BtL2capBackend &&other) noexcept {
  if (this != &other) {
    close_if_open(interrupt_fd_); close_if_open(control_fd_);
    address = std::move(other.address_);
    control_fd = other.control_fd_; interrupt_fd_ = other.interrupt_fd_;
    other.control_fd = -1; other.interrupt_fd_ = -1;
  }
  return *this;
}

void BtL2capBackend::send_output_report(std::span<const std::uint8_t> report) {
  if (interrupt_fd_ < 0) return;
  send_framed_packet(interrupt_fd_, hidp_output_packet(report));
}

bool BtL2capBackend::try_send_output_report(std::span<const std::uint8_t> report) {
  if (interrupt_fd_ < 0) return false;
  return send_framed_packet(interrupt_fd_, hidp_output_packet(report));
}

void BtL2capBackend::send_feature_get(std::uint8_t report_id) {
  if (control_fd_ < 0) return;
  send_framed_packet(control_fd_, feature_get_packet(report_id));
}

void BtL2capBackend::send_feature_set(std::span<const std::uint8_t> report) {
  if (control_fd_ < 0) return;
  auto packet = feature_set_packet(report);
  if (!packet.empty()) send_framed_packet(control_fd_, packet);
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_feature_report() {
  auto packet = read_framed_packet(control_fd_);
  if (!packet || packet->empty()) return std::nullopt;
  if (packet->at(0) == 0xA3) {
      return bt_feature_to_usb_feature_reply(std::span(packet->data() + 1, packet->size() - 1));
  }
  return bt_feature_to_usb_feature_reply(std::span(packet->data(), packet->size()));
}

std::optional<std::vector<std::uint8_t>> BtL2capBackend::read_interrupt_packet() {
  return read_framed_packet(interrupt_fd_);
}
} // namespace vds
