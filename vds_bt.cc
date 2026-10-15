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
  other.control_fd_ = -1; other.interrupt_fd_ = -1; 
}

BtL2capBackend &BtL2capBackend::operator=(BtL2capBackend &&other) noexcept {
  if (this != &other) {
    close_if_open(interrupt_fd_); close_if_open(control_fd_);
    address_ = std::move(other.address_);
    control_fd_ = other.control_fd_; interrupt_fd_ = other.interrupt_fd_;
    other.control_fd_ = -1; other.interrupt_fd_ = -1;
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
