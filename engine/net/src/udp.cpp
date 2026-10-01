#include "imperivm/net/udp.hpp"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace imperivm::net {
namespace {

#ifdef _WIN32
struct WinsockInit {
  WinsockInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockInit() { WSACleanup(); }
};
void ensure_started() { static WinsockInit init; }
void close_handle(std::intptr_t handle) { closesocket(static_cast<SOCKET>(handle)); }
#else
void ensure_started() {}
void close_handle(std::intptr_t handle) { ::close(static_cast<int>(handle)); }
#endif

sockaddr_in to_sockaddr(const Endpoint& endpoint) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(endpoint.address);
  addr.sin_port = htons(endpoint.port);
  return addr;
}

/// The largest datagram accepted. The link layer packs to 1200 bytes; one
/// oversize turn packet may exceed it, and IPv4 caps a datagram at 65507.
constexpr std::size_t kMaxDatagram = 65536;

}  // namespace

std::string Endpoint::str() const {
  return std::to_string((address >> 24) & 0xFF) + "." + std::to_string((address >> 16) & 0xFF) +
         "." + std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF) +
         ":" + std::to_string(port);
}

std::optional<Endpoint> resolve(const std::string& text, std::uint16_t default_port) {
  ensure_started();
  std::string host = text;
  std::uint16_t port = default_port;
  if (const std::size_t colon = text.rfind(':'); colon != std::string::npos) {
    host = text.substr(0, colon);
    const unsigned long parsed = std::strtoul(text.c_str() + colon + 1, nullptr, 10);
    if (parsed == 0 || parsed > 65535) return std::nullopt;
    port = static_cast<std::uint16_t>(parsed);
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
    return std::nullopt;
  }
  Endpoint endpoint;
  endpoint.address = ntohl(reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
  endpoint.port = port;
  freeaddrinfo(result);
  return endpoint;
}

std::optional<UdpSocket> UdpSocket::open(std::uint16_t port, bool loopback_only,
                                         std::string* error) {
  ensure_started();
  const auto fail = [error](const char* what) -> std::optional<UdpSocket> {
    if (error != nullptr) *error = what;
    return std::nullopt;
  };
#ifdef _WIN32
  const SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw == INVALID_SOCKET) return fail("socket() failed");
  const std::intptr_t handle = static_cast<std::intptr_t>(raw);
#else
  const int raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw < 0) return fail("socket() failed");
  const std::intptr_t handle = raw;
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(static_cast<decltype(raw)>(handle), reinterpret_cast<const sockaddr*>(&addr),
             sizeof(addr)) != 0) {
    close_handle(handle);
    return fail("bind() failed: is the port in use?");
  }
#ifdef _WIN32
  u_long nonblocking = 1;
  ioctlsocket(raw, FIONBIO, &nonblocking);
#else
  fcntl(raw, F_SETFL, fcntl(raw, F_GETFL, 0) | O_NONBLOCK);
#endif
  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  getsockname(static_cast<decltype(raw)>(handle), reinterpret_cast<sockaddr*>(&bound), &length);
  return UdpSocket(handle, ntohs(bound.sin_port));
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_), port_(other.port_) {
  other.handle_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    if (handle_ >= 0) close_handle(handle_);
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = -1;
  }
  return *this;
}

UdpSocket::~UdpSocket() {
  if (handle_ >= 0) close_handle(handle_);
}

bool UdpSocket::send(const Endpoint& to, std::span<const std::byte> bytes) {
  const sockaddr_in addr = to_sockaddr(to);
#ifdef _WIN32
  const auto sent = ::sendto(static_cast<SOCKET>(handle_), reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size()), 0,
                             reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
#else
  const auto sent = ::sendto(static_cast<int>(handle_), bytes.data(), bytes.size(), 0,
                             reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
#endif
  return sent == static_cast<decltype(sent)>(bytes.size());
}

std::optional<Received> UdpSocket::receive() {
  std::vector<std::byte> buffer(kMaxDatagram);
  sockaddr_in from{};
  socklen_t length = sizeof(from);
#ifdef _WIN32
  const auto got = ::recvfrom(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(buffer.data()),
                              static_cast<int>(buffer.size()), 0,
                              reinterpret_cast<sockaddr*>(&from), &length);
#else
  const auto got = ::recvfrom(static_cast<int>(handle_), buffer.data(), buffer.size(), 0,
                              reinterpret_cast<sockaddr*>(&from), &length);
#endif
  if (got <= 0) return std::nullopt;
  buffer.resize(static_cast<std::size_t>(got));
  Received received;
  received.from.address = ntohl(from.sin_addr.s_addr);
  received.from.port = ntohs(from.sin_port);
  received.bytes = std::move(buffer);
  return received;
}

void UdpSocket::enable_broadcast() {
  int on = 1;
#ifdef _WIN32
  setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_BROADCAST,
             reinterpret_cast<const char*>(&on), sizeof(on));
#else
  setsockopt(static_cast<int>(handle_), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
}

void UdpSocket::wait(int ms) {
#ifdef _WIN32
  fd_set read;
  FD_ZERO(&read);
  FD_SET(static_cast<SOCKET>(handle_), &read);
  timeval timeout{ms / 1000, (ms % 1000) * 1000};
  ::select(0, &read, nullptr, nullptr, &timeout);
#else
  pollfd fd{static_cast<int>(handle_), POLLIN, 0};
  ::poll(&fd, 1, ms);
#endif
}

std::uint32_t local_address() {
  ensure_started();
  std::uint32_t found = 0x7F000001u;
#ifdef _WIN32
  const SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw == INVALID_SOCKET) return found;
#else
  const int raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw < 0) return found;
#endif
  sockaddr_in far{};
  far.sin_family = AF_INET;
  far.sin_addr.s_addr = htonl(0x08080808u);  // any routable address; nothing is sent
  far.sin_port = htons(53);
  if (::connect(raw, reinterpret_cast<const sockaddr*>(&far), sizeof(far)) == 0) {
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (getsockname(raw, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
        local.sin_addr.s_addr != 0) {
      found = ntohl(local.sin_addr.s_addr);
    }
  }
  close_handle(static_cast<std::intptr_t>(raw));
  return found;
}

std::uint32_t now_ms() {
  const auto since = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

}  // namespace imperivm::net
