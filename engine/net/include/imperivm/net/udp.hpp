#pragma once

// A UDP socket and a millisecond clock: the two things a networked match needs
// from the operating system, and nothing more.
//
// IPv4 only, for now. Nothing above this file knows an address's shape --
// the link layer names peers by player slot -- so IPv6 is a change here and
// nowhere else. POSIX sockets; Winsock behind `_WIN32`, which compiles by
// construction and has not been run.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace imperivm::net {

struct Endpoint {
  std::uint32_t address = 0;  ///< host byte order
  std::uint16_t port = 0;

  [[nodiscard]] bool operator==(const Endpoint&) const noexcept = default;
  [[nodiscard]] std::string str() const;
};

/// `host:port`, or `host` with `default_port`. Resolves names.
[[nodiscard]] std::optional<Endpoint> resolve(const std::string& text,
                                              std::uint16_t default_port);

struct Received {
  Endpoint from;
  std::vector<std::byte> bytes;
};

class UdpSocket {
 public:
  /// Bound to `port` (zero for any) on every interface, or on loopback only.
  /// Non-blocking. Null on failure, with the reason in `error`.
  [[nodiscard]] static std::optional<UdpSocket> open(std::uint16_t port, bool loopback_only,
                                                     std::string* error = nullptr);

  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  ~UdpSocket();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  /// Best effort, like the protocol: a datagram the kernel will not take is a
  /// datagram lost, and the link layer already repairs those.
  bool send(const Endpoint& to, std::span<const std::byte> bytes);
  /// The next datagram waiting, or nothing.
  [[nodiscard]] std::optional<Received> receive();
  /// Allow sends to the broadcast address. Only the LAN browser asks.
  void enable_broadcast();
  /// Block until a datagram is waiting or `ms` pass. The one wait in a match
  /// loop, so it does not spin.
  void wait(int ms);

 private:
  explicit UdpSocket(std::intptr_t handle, std::uint16_t port) noexcept
      : handle_(handle), port_(port) {}
  std::intptr_t handle_ = -1;
  std::uint16_t port_ = 0;
};

/// This machine's address on the network it would use to reach the
/// internet, for a host to show its players; loopback when there is none.
/// Found by routing, not by sending: a datagram socket is connected to a
/// public address and asked which local one the system chose.
[[nodiscard]] std::uint32_t local_address();

/// Milliseconds on a steady clock, wrapping at 2^32. The link layer does its
/// arithmetic modulo that, so the wrap every 49 days is not an event.
[[nodiscard]] std::uint32_t now_ms();

}  // namespace imperivm::net
