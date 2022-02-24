// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These tests ensure fdio can talk to netstack.
// No network connection is required, only a running netstack binary.

#include <arpa/inet.h>
#include <fcntl.h>
#include <lib/fit/defer.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <zircon/compiler.h>

#include <array>
#include <future>
#include <latch>
#include <random>
#include <thread>

#include <fbl/unique_fd.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "util.h"

#if defined(__Fuchsia__)
#include <fidl/fuchsia.posix.socket/cpp/wire.h>
#include <lib/fdio/fd.h>

#include "src/lib/testing/predicates/status.h"
#endif

// TODO(C++20): Remove this; std::chrono::duration defines operator<< in c++20. See
// https://en.cppreference.com/w/cpp/chrono/duration/operator_ltlt.
namespace std::chrono {
template <class Rep, class Period>
void PrintTo(const std::chrono::duration<Rep, Period>& duration, std::ostream* os) {
  *os << std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() << "ns";
}
}  // namespace std::chrono

namespace {

#if defined(__Fuchsia__)

void ZxSocketInfo(int fd, zx_info_socket_t& out_info) {
  fidl::ClientEnd<fuchsia_posix_socket::StreamSocket> client_end;
  ASSERT_OK(fdio_fd_clone(fd, client_end.channel().reset_and_get_address()));
  fidl::WireSyncClient client = fidl::BindSyncClient(std::move(client_end));

  auto response = client->Describe();
  ASSERT_OK(response.status());
  const fuchsia_io::wire::NodeInfo& node_info = response.Unwrap()->info;
  ASSERT_EQ(node_info.Which(), fuchsia_io::wire::NodeInfo::Tag::kStreamSocket);

  ASSERT_OK(zx_object_get_info(node_info.stream_socket().socket.get(), ZX_INFO_SOCKET, &out_info,
                               sizeof(zx_info_socket_t), nullptr, nullptr));
}

#endif

template <typename T>
void AssertBlocked(const std::future<T>& fut) {
  // Give an asynchronous blocking operation some time to reach the blocking state. Clocks
  // sometimes jump in infrastructure, which may cause a single wait to trip sooner than expected,
  // without the asynchronous task getting a meaningful shot at running. We protect against that by
  // splitting the wait into multiple calls as an attempt to guarantee that clock jumps do not
  // impact the duration of a wait.
  for (int i = 0; i < 50; i++) {
    ASSERT_EQ(fut.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
  }
}

void AssertExpectedReventsAfterPeerShutdown(int fd) {
  pollfd pfd = {
      .fd = fd,
      // POLLOUT is masked because otherwise the `poll()` will return immediately,
      // before shutdown is complete. POLLWRNORM and POLLRDNORM are masked because
      // we do not yet support them on Fuchsia.
      //
      // TODO(https://fxbug.dev/73258): Support POLLWRNORM and POLLRDNORM on Fuchsia.
      .events =
          std::numeric_limits<decltype(pfd.events)>::max() & ~(POLLOUT | POLLWRNORM | POLLRDNORM),
  };

  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  EXPECT_GE(n, 0) << strerror(errno);
  EXPECT_EQ(n, 1);

#if defined(__Fuchsia__)
  EXPECT_EQ(pfd.revents, POLLERR | POLLHUP | POLLRDHUP | POLLIN);
#else
  // Prior to this commit[1], Linux sometimes returns a subset of the expected `revents`
  // when the client `poll`s after the receipt of a TCP RST message.
  //
  // TODO(https://fxbug.dev/87541): Match Fuchsia after Linux version is >= 4.12.
  //
  // [1]: https://github.com/torvalds/linux/commit/3d4762639dd36a5f0f433f0c9d82e9743dc21a33
  EXPECT_THAT(pfd.revents, testing::AnyOf(testing::Eq(POLLERR), testing::Eq(POLLERR | POLLHUP),
                                          testing::Eq(POLLERR | POLLHUP | POLLRDHUP | POLLIN)));
#endif
}

void SocketType(int fd, uint32_t& sock_type) {
  socklen_t socktype_optlen = sizeof(sock_type);
  ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &socktype_optlen), 0)
      << strerror(errno);
  ASSERT_EQ(socktype_optlen, sizeof(sock_type));
}

void TxCapacity(int fd, size_t& out_capacity) {
  uint32_t sndbuf_opt;
  socklen_t sndbuf_optlen = sizeof(sndbuf_opt);
  ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf_opt, &sndbuf_optlen), 0)
      << strerror(errno);
  ASSERT_EQ(sndbuf_optlen, sizeof(sndbuf_opt));

  // SO_SNDBUF lies and reports double the real value.
  out_capacity = sndbuf_opt >> 1;

  uint32_t sock_type;
  ASSERT_NO_FATAL_FAILURE(SocketType(fd, sock_type));

#if defined(__Fuchsia__)
  if (sock_type == SOCK_STREAM) {
    // TODO(https://fxbug.dev/60337): We can avoid this additional space once zircon sockets are
    // not artificially increasing the buffer sizes.
    zx_info_socket_t zx_socket_info;
    ASSERT_NO_FATAL_FAILURE(ZxSocketInfo(fd, zx_socket_info));
    out_capacity += zx_socket_info.tx_buf_max;
  }
#endif
}

void RxCapacity(int fd, size_t& out_capacity) {
  uint32_t rcvbuf_opt;
  socklen_t rcvbuf_optlen = sizeof(rcvbuf_opt);
  ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_opt, &rcvbuf_optlen), 0)
      << strerror(errno);
  ASSERT_EQ(rcvbuf_optlen, sizeof(rcvbuf_opt));

  // SO_RCVBUF lies and reports double the real value.
  out_capacity = rcvbuf_opt >> 1;

  uint32_t sock_type;
  ASSERT_NO_FATAL_FAILURE(SocketType(fd, sock_type));

#if defined(__Fuchsia__)
  if (sock_type == SOCK_STREAM) {
    // TODO(https://fxbug.dev/60337): We can avoid this additional space once zircon sockets are
    // not artificially increasing the buffer sizes.
    zx_info_socket_t zx_socket_info;
    ASSERT_NO_FATAL_FAILURE(ZxSocketInfo(fd, zx_socket_info));
    out_capacity += zx_socket_info.rx_buf_max;
  }
#endif
}

TEST(LocalhostTest, SendToZeroPort) {
  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(0),
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(sendto(fd.get(), nullptr, 0, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            -1);
  ASSERT_EQ(errno, EINVAL) << strerror(errno);

  addr.sin_port = htons(1234);
  ASSERT_EQ(sendto(fd.get(), nullptr, 0, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            0)
      << strerror(errno);
}

TEST(LocalhostTest, DatagramSocketIgnoresMsgWaitAll) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  ASSERT_EQ(recvfrom(recvfd.get(), nullptr, 0, MSG_WAITALL, nullptr, nullptr), -1);
  ASSERT_EQ(errno, EAGAIN) << strerror(errno);

  ASSERT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, DatagramSocketSendMsgNameLenTooBig) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
  };

  msghdr msg = {
      .msg_name = &addr,
      .msg_namelen = sizeof(sockaddr_storage) + 1,
  };

  ASSERT_EQ(sendmsg(fd.get(), &msg, 0), -1);
  ASSERT_EQ(errno, EINVAL) << strerror(errno);

  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, DatagramSocketAtOOBMark) {
  fbl::unique_fd client;
  ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  // sockatmark is not supported on datagram sockets on Linux or Fuchsia.
  // It is on macOS.
  EXPECT_EQ(sockatmark(client.get()), -1);
  // This should be ENOTTY per POSIX:
  // https://pubs.opengroup.org/onlinepubs/9699919799/functions/sockatmark.html
  EXPECT_EQ(errno, ENOTTY) << strerror(errno);
}

#if !defined(__Fuchsia__)
bool IsRoot() {
  uid_t ruid, euid, suid;
  EXPECT_EQ(getresuid(&ruid, &euid, &suid), 0) << strerror(errno);
  gid_t rgid, egid, sgid;
  EXPECT_EQ(getresgid(&rgid, &egid, &sgid), 0) << strerror(errno);
  auto uids = {ruid, euid, suid};
  auto gids = {rgid, egid, sgid};
  return std::all_of(std::begin(uids), std::end(uids), [](uid_t uid) { return uid == 0; }) &&
         std::all_of(std::begin(gids), std::end(gids), [](gid_t gid) { return gid == 0; });
}
#endif

TEST(LocalhostTest, BindToDevice) {
#if !defined(__Fuchsia__)
  if (!IsRoot()) {
    GTEST_SKIP() << "This test requires root";
  }
#endif

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  {
    // The default is that a socket is not bound to a device.
    char get_dev[IFNAMSIZ] = {};
    socklen_t get_dev_length = sizeof(get_dev);
    EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, get_dev, &get_dev_length), 0)
        << strerror(errno);
    EXPECT_EQ(get_dev_length, socklen_t(0));
    EXPECT_STREQ(get_dev, "");
  }

  const char set_dev[IFNAMSIZ] = "lo\0blahblah";

  // Bind to "lo" with null termination should work even if the size is too big.
  ASSERT_EQ(setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, set_dev, sizeof(set_dev)), 0)
      << strerror(errno);

  const char set_dev_unknown[] = "loblahblahblah";
  // Bind to "lo" without null termination but with accurate length should work.
  EXPECT_EQ(setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, set_dev_unknown, 2), 0)
      << strerror(errno);

  // Bind to unknown name should fail.
  EXPECT_EQ(
      setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, "loblahblahblah", sizeof(set_dev_unknown)),
      -1);
  EXPECT_EQ(errno, ENODEV) << strerror(errno);

  {
    // Reading it back should work.
    char get_dev[IFNAMSIZ] = {};
    socklen_t get_dev_length = sizeof(get_dev);
    EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, get_dev, &get_dev_length), 0)
        << strerror(errno);
    EXPECT_EQ(get_dev_length, strlen(set_dev) + 1);
    EXPECT_STREQ(get_dev, set_dev);
  }

  {
    // Reading it back without enough space in the buffer should fail.
    char get_dev[] = "";
    socklen_t get_dev_length = sizeof(get_dev);
    EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, get_dev, &get_dev_length), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
    EXPECT_EQ(get_dev_length, sizeof(get_dev));
    EXPECT_STREQ(get_dev, "");
  }

  EXPECT_EQ(close(fd.release()), 0) << strerror(errno);
}

// Test the error when a client's sandbox does not have access raw/packet sockets.
TEST(LocalhostTest, RawSocketsNotAvailable) {
  // No raw INET sockets.
  ASSERT_EQ(socket(AF_INET, SOCK_RAW, 0), -1);
  ASSERT_EQ(errno, EPROTONOSUPPORT) << strerror(errno);
  ASSERT_EQ(socket(AF_INET, SOCK_RAW, IPPROTO_UDP), -1);
  ASSERT_EQ(errno, EPERM) << strerror(errno);
  ASSERT_EQ(socket(AF_INET, SOCK_RAW, IPPROTO_RAW), -1);
  ASSERT_EQ(errno, EPERM) << strerror(errno);

  // No packet sockets.
  ASSERT_EQ(socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)), -1);
  ASSERT_EQ(errno, EPERM) << strerror(errno);
}

TEST(LocalhostTest, IpAddMembershipAny) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  ip_mreqn param = {
      .imr_address =
          {
              .s_addr = htonl(INADDR_ANY),
          },
      .imr_ifindex = 1,
  };
  int n = inet_pton(AF_INET, "224.0.2.1", &param.imr_multiaddr.s_addr);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  ASSERT_EQ(setsockopt(s.get(), SOL_IP, IP_ADD_MEMBERSHIP, &param, sizeof(param)), 0)
      << strerror(errno);

  ASSERT_EQ(close(s.release()), 0) << strerror(errno);
}

// TODO(https://fxbug.dev/90038): Delete once SockOptsTest is gone.
struct SockOption {
  int level;
  int option;

  bool operator==(const SockOption& other) const {
    return level == other.level && option == other.option;
  }
};

constexpr int INET_ECN_MASK = 3;

std::string socketDomainToString(const int domain) {
  switch (domain) {
    case AF_INET:
      return "IPv4";
    case AF_INET6:
      return "IPv6";
    default:
      return std::to_string(domain);
  }
}

std::string socketTypeToString(const int type) {
  switch (type) {
    case SOCK_DGRAM:
      return "Datagram";
    case SOCK_STREAM:
      return "Stream";
    default:
      return std::to_string(type);
  }
}

using SocketKind = std::tuple<int, int>;

std::string SocketKindToString(const testing::TestParamInfo<SocketKind>& info) {
  auto const& [domain, type] = info.param;
  return socketDomainToString(domain) + "_" + socketTypeToString(type);
}

// Share common functions for SocketKind based tests.
class SocketKindTest : public testing::TestWithParam<SocketKind> {
 protected:
  static fbl::unique_fd NewSocket() {
    auto const& [domain, type] = GetParam();
    return fbl::unique_fd(socket(domain, type, 0));
  }

  static void LoopbackAddr(sockaddr_storage* ss, socklen_t* len) {
    auto const& [domain, protocol] = GetParam();

    switch (domain) {
      case AF_INET:
        *(reinterpret_cast<sockaddr_in*>(ss)) = {
            .sin_family = AF_INET,
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
        };
        *len = sizeof(sockaddr_in);
        break;
      case AF_INET6:
        *(reinterpret_cast<sockaddr_in6*>(ss)) = {
            .sin6_family = AF_INET6,
            .sin6_addr = IN6ADDR_LOOPBACK_INIT,
        };
        *len = sizeof(sockaddr_in6);
        break;
      default:
        FAIL() << "unexpected domain = " << domain;
        break;
    }
  }
};

constexpr int kSockOptOn = 1;
constexpr int kSockOptOff = 0;

struct SocketOption {
  int level;
  std::string level_str;
  int name;
  std::string name_str;
};

#define STRINGIFIED_SOCKOPT(level, name) \
  SocketOption { level, #level, name, #name }

struct IntSocketOption {
  SocketOption option;
  bool is_boolean;
  int default_value;
  std::vector<int> valid_values;
  std::vector<int> invalid_values;
};

class SocketOptionTestBase : public testing::Test {
 public:
  SocketOptionTestBase(int domain, int type) : sock_domain_(domain), sock_type_(type) {}

 protected:
  void SetUp() override {
    ASSERT_TRUE(sock_ = fbl::unique_fd(socket(sock_domain_, sock_type_, 0))) << strerror(errno);
  }

  void TearDown() override { EXPECT_EQ(close(sock_.release()), 0) << strerror(errno); }

  bool IsOptionLevelSupportedByDomain(int level) const {
#if defined(__Fuchsia__)
    // TODO(https://gvisor.dev/issues/6389): Remove once Fuchsia returns an error
    // when setting/getting IPv6 options on an IPv4 socket.
    return true;
#else
    // IPv6 options are only supported on AF_INET6 sockets.
    return sock_domain_ == AF_INET6 || level != IPPROTO_IPV6;
#endif
  }

  fbl::unique_fd const& sock() const { return sock_; }

 private:
  fbl::unique_fd sock_;
  const int sock_domain_;
  const int sock_type_;
};

std::string socketKindAndOptionToString(int domain, int type, SocketOption opt) {
  std::ostringstream oss;
  oss << socketDomainToString(domain);
  oss << '_' << socketTypeToString(type);
  oss << '_' << opt.level_str;
  oss << '_' << opt.name_str;
  return oss.str();
}

using SocketKindAndIntOption = std::tuple<int, int, IntSocketOption>;

std::string SocketKindAndIntOptionToString(
    const testing::TestParamInfo<SocketKindAndIntOption>& info) {
  auto const& [domain, type, int_opt] = info.param;
  return socketKindAndOptionToString(domain, type, int_opt.option);
}

// Test functionality common to every integer and pseudo-boolean socket option.
class IntSocketOptionTest : public SocketOptionTestBase,
                            public testing::WithParamInterface<SocketKindAndIntOption> {
 protected:
  IntSocketOptionTest()
      : SocketOptionTestBase(std::get<0>(GetParam()), std::get<1>(GetParam())),
        opt_(std::get<2>(GetParam())) {}

  void SetUp() override {
    ASSERT_FALSE(opt_.valid_values.empty()) << "must have at least one valid value";
    SocketOptionTestBase::SetUp();
  }

  void TearDown() override { SocketOptionTestBase::TearDown(); }

  bool IsOptionCharCompatible() const {
    const int level = opt_.option.level;
    return level != IPPROTO_IPV6 && level != SOL_SOCKET;
  }

  IntSocketOption const& opt() const { return opt_; }

 private:
  const IntSocketOption opt_;
};

TEST_P(IntSocketOptionTest, Default) {
  int get = -1;
  socklen_t get_len = sizeof(get);
  const int r = getsockopt(sock().get(), opt().option.level, opt().option.name, &get, &get_len);

  if (IsOptionLevelSupportedByDomain(opt().option.level)) {
    ASSERT_EQ(r, 0) << strerror(errno);
    ASSERT_EQ(get_len, sizeof(get));
    EXPECT_EQ(get, opt().default_value);
  } else {
    ASSERT_EQ(r, -1);
    EXPECT_EQ(errno, ENOTSUP) << strerror(errno);
  }
}

TEST_P(IntSocketOptionTest, SetValid) {
  for (int value : opt().valid_values) {
    SCOPED_TRACE("value=" + std::to_string(value));
    // Test each value in a lambda so we continue testing the other values if an ASSERT fails.
    [&]() {
      const int r =
          setsockopt(sock().get(), opt().option.level, opt().option.name, &value, sizeof(value));

      if (IsOptionLevelSupportedByDomain(opt().option.level)) {
        ASSERT_EQ(r, 0) << strerror(errno);
        int get = -1;
        socklen_t get_len = sizeof(get);
        ASSERT_EQ(getsockopt(sock().get(), opt().option.level, opt().option.name, &get, &get_len),
                  0)
            << strerror(errno);
        ASSERT_EQ(get_len, sizeof(get));
        EXPECT_EQ(get, opt().is_boolean ? static_cast<bool>(value) : value);
      } else {
        ASSERT_EQ(r, -1);
        EXPECT_EQ(errno, ENOPROTOOPT) << strerror(errno);
      }
    }();
  }
}

TEST_P(IntSocketOptionTest, SetInvalid) {
  for (int value : opt().invalid_values) {
    SCOPED_TRACE("value=" + std::to_string(value));
    // Test each value in a lambda so we continue testing the other values if an ASSERT fails.
    [&]() {
      const int r =
          setsockopt(sock().get(), opt().option.level, opt().option.name, &value, sizeof(value));

      if (IsOptionLevelSupportedByDomain(opt().option.level)) {
        ASSERT_EQ(r, -1);
        EXPECT_EQ(errno, EINVAL) << strerror(errno);

        // Confirm that no changes were made.
        int get = -1;
        socklen_t get_len = sizeof(get);
        ASSERT_EQ(getsockopt(sock().get(), opt().option.level, opt().option.name, &get, &get_len),
                  0)
            << strerror(errno);
        ASSERT_EQ(get_len, sizeof(get));
        EXPECT_EQ(get, opt().default_value);
      } else {
        ASSERT_EQ(r, -1);
        EXPECT_EQ(errno, ENOPROTOOPT) << strerror(errno);
      }
    }();
  }
}

TEST_P(IntSocketOptionTest, SetChar) {
  for (int value : opt().valid_values) {
    SCOPED_TRACE("value=" + std::to_string(value));
    // Test each value in a lambda so we continue testing the other values if an ASSERT fails.
    [&]() {
      int want;
      {
        const char set_char = static_cast<char>(value);
        if (static_cast<int>(set_char) != value) {
          // Skip values that don't fit in a char.
          return;
        }
        const int r = setsockopt(sock().get(), opt().option.level, opt().option.name, &set_char,
                                 sizeof(set_char));
        if (!IsOptionLevelSupportedByDomain(opt().option.level)) {
          ASSERT_EQ(r, -1);
          EXPECT_EQ(errno, ENOPROTOOPT) << strerror(errno);
          want = opt().default_value;
        } else if (!IsOptionCharCompatible()) {
          ASSERT_EQ(r, -1);
          EXPECT_EQ(errno, EINVAL) << strerror(errno);
          want = opt().default_value;
        } else {
          ASSERT_EQ(r, 0) << strerror(errno);
          want = opt().is_boolean ? static_cast<bool>(set_char) : set_char;
        }
      }

      {
        char get = -1;
        socklen_t get_len = sizeof(get);
        const int r =
            getsockopt(sock().get(), opt().option.level, opt().option.name, &get, &get_len);
        if (!IsOptionLevelSupportedByDomain(opt().option.level)) {
          ASSERT_EQ(r, -1);
          EXPECT_EQ(errno, ENOTSUP) << strerror(errno);
        } else {
          ASSERT_EQ(r, 0) << strerror(errno);
          ASSERT_EQ(get_len, sizeof(get));
          EXPECT_EQ(get, static_cast<char>(want));
        }
      }

      {
        int16_t get = -1;
        socklen_t get_len = sizeof(get);
        const int r =
            getsockopt(sock().get(), opt().option.level, opt().option.name, &get, &get_len);
        if (!IsOptionLevelSupportedByDomain(opt().option.level)) {
          ASSERT_EQ(r, -1);
          EXPECT_EQ(errno, ENOTSUP) << strerror(errno);
        } else if (!IsOptionCharCompatible()) {
          ASSERT_EQ(r, 0) << strerror(errno);
          ASSERT_EQ(get_len, sizeof(get));
          EXPECT_EQ(get, want);
        } else {
          ASSERT_EQ(r, 0) << strerror(errno);
          // Truncates size < 4 to 1 and only writes the low byte.
          // https://github.com/torvalds/linux/blob/2585cf9dfaa/net/ipv4/ip_sockglue.c#L1742-L1745
          ASSERT_EQ(get_len, sizeof(char));
          EXPECT_EQ(get, static_cast<int16_t>(uint16_t(-1) << 8) | want);
        }
      }

      {
        int get = -1;
        socklen_t get_len = sizeof(get);
        const int r =
            getsockopt(sock().get(), opt().option.level, opt().option.name, &get, &get_len);
        if (!IsOptionLevelSupportedByDomain(opt().option.level)) {
          ASSERT_EQ(r, -1);
          EXPECT_EQ(errno, ENOTSUP) << strerror(errno);
        } else {
          ASSERT_EQ(r, 0) << strerror(errno);
          ASSERT_EQ(get_len, sizeof(get));
          EXPECT_EQ(get, want);
        }
      }
    }();
  }
}

const std::vector<int> kBooleanOptionValidValues = {-2, -1, 0, 1, 2, 15, 255, 256};

// The tests below use valid and invalid values that attempt to cover normal use cases,
// min/max values, and invalid negative/large values.
// Special values (e.g. ones that reset an option to its default) have option-specific tests.
INSTANTIATE_TEST_SUITE_P(
    IntSocketOptionTests, IntSocketOptionTest,
    testing::Combine(testing::Values(AF_INET, AF_INET6), testing::Values(SOCK_STREAM, SOCK_DGRAM),
                     testing::Values(
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IP, IP_MULTICAST_LOOP),
                             .is_boolean = true,
                             .default_value = 1,
                             .valid_values = kBooleanOptionValidValues,
                             .invalid_values = {},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IP, IP_TOS),
                             .is_boolean = false,
                             .default_value = 0,
                             // The ECN (2 rightmost) bits may be cleared, so we use arbitrary
                             // values without these bits set. See CheckSkipECN test.
                             .valid_values = {0x04, 0xC0, 0xFC},
                             // Larger-than-byte values are accepted but the extra bits are
                             // merely ignored. See InvalidLargeTOS test.
                             .invalid_values = {},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IP, IP_RECVTOS),
                             .is_boolean = true,
                             .default_value = 0,
                             .valid_values = kBooleanOptionValidValues,
                             .invalid_values = {},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IP, IP_TTL),
                             .is_boolean = false,
                             .default_value = 64,
                             // -1 is not tested here, it is a special value which resets ttl to
                             // its default value.
                             .valid_values = {1, 2, 15, 255},
                             .invalid_values = {-2, 0, 256},
                         },
                         IntSocketOption {
                           .option = STRINGIFIED_SOCKOPT(IPPROTO_IPV6, IPV6_MULTICAST_LOOP),
                           .is_boolean = true, .default_value = 1,
#if defined(__Fuchsia__)
                           .valid_values = kBooleanOptionValidValues, .invalid_values = {},
#else
                           // On Linux, this option only accepts 0 or 1. This is one of a kind.
                           // There seem to be no good reasons for it, so it should probably be
                           // fixed in Linux rather than in Fuchsia.
                           // https://github.com/torvalds/linux/blob/eec4df26e24/net/ipv6/ipv6_sockglue.c#L758
                               .valid_values = {0, 1}, .invalid_values = {-2, -1, 2, 15, 255, 256},
#endif
                         },
                         IntSocketOption {
                           .option = STRINGIFIED_SOCKOPT(IPPROTO_IPV6, IPV6_TCLASS),
                           .is_boolean = false, .default_value = 0,
#if defined(__Fuchsia__)
                           // TODO(https://gvisor.dev/issues/6389): Remove once Fuchsia treats
                           // IPV6_TCLASS differently than IP_TOS. See CheckSkipECN test.
                               .valid_values = {0x04, 0xC0, 0xFC},
#else
                           // -1 is not tested here, it is a special value which resets the traffic
                           // class to its default value.
                               .valid_values = {0, 1, 2, 15, 255},
#endif
                           .invalid_values = {-2, 256},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IPV6, IPV6_RECVTCLASS),
                             .is_boolean = true,
                             .default_value = 0,
                             .valid_values = kBooleanOptionValidValues,
                             .invalid_values = {},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IPV6, IPV6_UNICAST_HOPS),
                             .is_boolean = false,
                             .default_value = 64,
                             // -1 is not tested here, it is a special value which resets ttl to
                             // its default value.
                             .valid_values = {0, 1, 2, 15, 255},
                             .invalid_values = {-2, 256},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(SOL_SOCKET, SO_NO_CHECK),
                             .is_boolean = true,
                             .default_value = 0,
                             .valid_values = kBooleanOptionValidValues,
                             .invalid_values = {},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(SOL_SOCKET, SO_TIMESTAMP),
                             .is_boolean = true,
                             .default_value = 0,
                             .valid_values = kBooleanOptionValidValues,
                             .invalid_values = {},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(SOL_SOCKET, SO_TIMESTAMPNS),
                             .is_boolean = true,
                             .default_value = 0,
                             .valid_values = kBooleanOptionValidValues,
                             .invalid_values = {},
                         })),
    SocketKindAndIntOptionToString);

// TODO(https://github.com/google/gvisor/issues/6972): Test multicast ttl options on SOCK_STREAM
// sockets. Right now it's complicated because setting these options on a stream socket silently
// fails (no error returned but no change observed).
INSTANTIATE_TEST_SUITE_P(
    DatagramIntSocketOptionTests, IntSocketOptionTest,
    testing::Combine(testing::Values(AF_INET, AF_INET6), testing::Values(SOCK_DGRAM),
                     testing::Values(
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IP, IP_MULTICAST_TTL),
                             .is_boolean = false,
                             .default_value = 1,
                             // -1 is not tested here, it is a special value which
                             // resets the ttl to its default value.
                             .valid_values = {0, 1, 2, 15, 128, 255},
                             .invalid_values = {-2, 256},
                         },
                         IntSocketOption{
                             .option = STRINGIFIED_SOCKOPT(IPPROTO_IPV6, IPV6_MULTICAST_HOPS),
                             .is_boolean = false,
                             .default_value = 1,
                             // -1 is not tested here, it is a special value which
                             // resets the hop limit to its default value.
                             .valid_values = {0, 1, 2, 15, 128, 255},
                             .invalid_values = {-2, 256},
                         })),
    SocketKindAndIntOptionToString);

using SocketKindAndOption = std::tuple<int, int, SocketOption>;

std::string SocketKindAndOptionToString(const testing::TestParamInfo<SocketKindAndOption>& info) {
  auto const& [domain, type, opt] = info.param;
  return socketKindAndOptionToString(domain, type, opt);
}

class SocketOptionSharedTest : public SocketOptionTestBase,
                               public testing::WithParamInterface<SocketKindAndOption> {
 protected:
  SocketOptionSharedTest()
      : SocketOptionTestBase(std::get<0>(GetParam()), std::get<1>(GetParam())),
        opt_(std::get<2>(GetParam())) {}

  void SetUp() override { SocketOptionTestBase::SetUp(); }

  void TearDown() override { SocketOptionTestBase::TearDown(); }

  SocketOption opt() const { return opt_; }

 private:
  const SocketOption opt_;
};

using TtlHopLimitSocketOptionTest = SocketOptionSharedTest;

TEST_P(TtlHopLimitSocketOptionTest, ResetToDefault) {
  if (!IsOptionLevelSupportedByDomain(opt().level)) {
    GTEST_SKIP() << "Option not supported by socket domain";
  }

  constexpr int kDefaultTTL = 64;
  constexpr int kNonDefaultValue = kDefaultTTL + 1;
  ASSERT_EQ(setsockopt(sock().get(), opt().level, opt().name, &kNonDefaultValue,
                       sizeof(kNonDefaultValue)),
            0)
      << strerror(errno);

  // Coherence check.
  {
    int get = -1;
    socklen_t get_len = sizeof(get);
    ASSERT_EQ(getsockopt(sock().get(), opt().level, opt().name, &get, &get_len), 0)
        << strerror(errno);
    ASSERT_EQ(get_len, sizeof(get));
    EXPECT_EQ(get, kNonDefaultValue);
  }

  constexpr int kResetValue = -1;
  ASSERT_EQ(setsockopt(sock().get(), opt().level, opt().name, &kResetValue, sizeof(kResetValue)), 0)
      << strerror(errno);

  {
    int get = -1;
    socklen_t get_len = sizeof(get);
    ASSERT_EQ(getsockopt(sock().get(), opt().level, opt().name, &get, &get_len), 0)
        << strerror(errno);
    ASSERT_EQ(get_len, sizeof(get));
    EXPECT_EQ(get, kDefaultTTL);
  }
}

INSTANTIATE_TEST_SUITE_P(
    TtlHopLimitSocketOptionTests, TtlHopLimitSocketOptionTest,
    testing::Combine(testing::Values(AF_INET, AF_INET6), testing::Values(SOCK_DGRAM, SOCK_STREAM),
                     testing::Values(STRINGIFIED_SOCKOPT(IPPROTO_IP, IP_TTL),
                                     STRINGIFIED_SOCKOPT(IPPROTO_IPV6, IPV6_UNICAST_HOPS))),
    SocketKindAndOptionToString);

// TODO(https://fxbug.dev/90038): Use SocketOptionTestBase for these tests.
class SocketOptsTest : public SocketKindTest {
 protected:
  static bool IsTCP() { return std::get<1>(GetParam()) == SOCK_STREAM; }

  static bool IsIPv6() { return std::get<0>(GetParam()) == AF_INET6; }

  static SockOption GetTOSOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_TCLASS,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_TOS,
    };
  }

  static SockOption GetMcastTTLOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_MULTICAST_HOPS,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_MULTICAST_TTL,
    };
  }

  static SockOption GetMcastIfOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_MULTICAST_IF,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_MULTICAST_IF,
    };
  }

  static SockOption GetRecvTOSOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_RECVTCLASS,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_RECVTOS,
    };
  }

  constexpr static SockOption GetNoChecksum() {
    return {
        .level = SOL_SOCKET,
        .option = SO_NO_CHECK,
    };
  }

  constexpr static SockOption GetTimestamp() {
    return {
        .level = SOL_SOCKET,
        .option = SO_TIMESTAMP,
    };
  }

  constexpr static SockOption GetTimestampNs() {
    return {
        .level = SOL_SOCKET,
        .option = SO_TIMESTAMPNS,
    };
  }
};

TEST_P(SocketOptsTest, ResetTtlToDefault) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get1 = -1;
  socklen_t get1_sz = sizeof(get1);
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get1, &get1_sz), 0) << strerror(errno);
  EXPECT_EQ(get1_sz, sizeof(get1));

  int set1 = 100;
  if (set1 == get1) {
    set1 += 1;
  }
  socklen_t set1_sz = sizeof(set1);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set1, set1_sz), 0) << strerror(errno);

  int set2 = -1;
  socklen_t set2_sz = sizeof(set2);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set2, set2_sz), 0) << strerror(errno);

  int get2 = -1;
  socklen_t get2_sz = sizeof(get2);
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get2, &get2_sz), 0) << strerror(errno);
  EXPECT_EQ(get2_sz, sizeof(get2));
  EXPECT_EQ(get2, get1);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, NullTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  socklen_t set_sz = sizeof(int);
  SockOption t = GetTOSOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, nullptr, set_sz), 0) << strerror(errno);
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, nullptr, set_sz), -1);
    EXPECT_EQ(errno, EFAULT) << strerror(errno);
  }
  socklen_t get_sz = sizeof(int);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, nullptr, &get_sz), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  int get = -1;
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, nullptr), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, InvalidLargeTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  // Test with exceeding the byte space.
  int set = 256;
  constexpr int kDefaultTOS = 0;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    // Linux allows values larger than 255, though it only looks at the char part of the value.
    // https://github.com/torvalds/linux/blob/eec4df26e24/net/ipv4/ip_sockglue.c#L1047
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  }
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, kDefaultTOS);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, CheckSkipECN) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xFF;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  int expect = static_cast<uint8_t>(set);
  if (IsTCP()
#if !defined(__Fuchsia__)
      // gvisor-netstack`s implemention of setsockopt(..IPV6_TCLASS..)
      // clears the ECN bits from the TCLASS value. This keeps gvisor
      // in parity with the Linux test-hosts that run a custom kernel.
      // But that is not the behavior of vanilla Linux kernels.
      // This #if can be removed when we migrate away from gvisor-netstack.
      && !IsIPv6()
#endif
  ) {
    expect &= ~INET_ECN_MASK;
  }
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, expect);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, ZeroTOSOptionSize) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xC0;
  socklen_t set_sz = 0;
  SockOption t = GetTOSOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  }
  int get = -1;
  socklen_t get_sz = 0;
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, 0u);
  EXPECT_EQ(get, -1);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SmallTOSOptionSize) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xC0;
  constexpr int kDefaultTOS = 0;
  SockOption t = GetTOSOption();
  for (socklen_t i = 1; i < sizeof(int); i++) {
    int expect_tos;
    socklen_t expect_sz;
    if (IsIPv6()) {
      EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, i), -1);
      EXPECT_EQ(errno, EINVAL) << strerror(errno);
      expect_tos = kDefaultTOS;
      expect_sz = i;
    } else {
      EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, i), 0) << strerror(errno);
      expect_tos = set;
      expect_sz = sizeof(uint8_t);
    }
    uint get = -1;
    socklen_t get_sz = i;
    EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
    EXPECT_EQ(get_sz, expect_sz);
    // Account for partial copies by getsockopt, retrieve the lower
    // bits specified by get_sz, while comparing against expect_tos.
    EXPECT_EQ(get & ~(~0u << (get_sz * 8)), static_cast<uint>(expect_tos));
  }
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, LargeTOSOptionSize) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  char buffer[100];
  int* set = reinterpret_cast<int*>(buffer);
  // Point to a larger buffer so that the setsockopt does not overrun.
  *set = 0xC0;
  SockOption t = GetTOSOption();
  for (socklen_t i = sizeof(int); i < 10; i++) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, set, i), 0) << strerror(errno);
    int get = -1;
    socklen_t get_sz = i;
    // We expect the system call handler to only copy atmost sizeof(int) bytes
    // as asserted by the check below. Hence, we do not expect the copy to
    // overflow in getsockopt.
    EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
    EXPECT_EQ(get_sz, sizeof(int));
    EXPECT_EQ(get, *set);
  }
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, NegativeTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = -1;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  int expect;
  if (IsIPv6()) {
    // On IPv6 TCLASS, setting -1 has the effect of resetting the
    // TrafficClass.
    expect = 0;
  } else {
    expect = static_cast<uint8_t>(set);
    if (IsTCP()) {
      expect &= ~INET_ECN_MASK;
    }
  }
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, expect);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, InvalidNegativeTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = -2;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  int expect;
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
    expect = 0;
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
    expect = static_cast<uint8_t>(set);
    if (IsTCP()) {
      expect &= ~INET_ECN_MASK;
    }
  }
  int get = 0;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, expect);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLNegativeOne) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kArbitrary = 6;
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kArbitrary, sizeof(kArbitrary)), 0)
      << strerror(errno);

  constexpr int kNegOne = -1;
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kNegOne, sizeof(kNegOne)), 0)
      << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, 1);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastIfImrIfindex) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kOne = 1;
  SockOption t = GetMcastIfOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kOne, sizeof(kOne)), 0) << strerror(errno);

    int param_out;
    socklen_t len = sizeof(param_out);
    ASSERT_EQ(getsockopt(s.get(), t.level, t.option, &param_out, &len), 0) << strerror(errno);
    ASSERT_EQ(len, sizeof(param_out));

    ASSERT_EQ(param_out, kOne);
  } else {
    ip_mreqn param_in = {
        .imr_ifindex = kOne,
    };
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &param_in, sizeof(param_in)), 0)
        << strerror(errno);

    in_addr param_out;
    socklen_t len = sizeof(param_out);
    ASSERT_EQ(getsockopt(s.get(), t.level, t.option, &param_out, &len), 0) << strerror(errno);
    ASSERT_EQ(len, sizeof(param_out));

    ASSERT_EQ(param_out.s_addr, INADDR_ANY);
  }

  ASSERT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastIfImrAddress) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }
  if (IsIPv6()) {
    GTEST_SKIP() << "V6 sockets don't support setting IP_MULTICAST_IF by addr";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  SockOption t = GetMcastIfOption();
  ip_mreqn param_in = {
      .imr_address =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &param_in, sizeof(param_in)), 0)
      << strerror(errno);

  in_addr param_out;
  socklen_t len = sizeof(param_out);
  ASSERT_EQ(getsockopt(s.get(), t.level, t.option, &param_out, &len), 0) << strerror(errno);
  ASSERT_EQ(len, sizeof(param_out));

  ASSERT_EQ(param_out.s_addr, param_in.imr_address.s_addr);

  ASSERT_EQ(close(s.release()), 0) << strerror(errno);
}

// Tests that a two byte RECVTOS/RECVTCLASS optval is acceptable.
TEST_P(SocketOptsTest, SetReceiveTOSShort) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip receive TOS tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr char kSockOptOn2Byte[] = {kSockOptOn, 0};
  constexpr char kSockOptOff2Byte[] = {kSockOptOff, 0};

  SockOption t = GetRecvTOSOption();
  if (IsIPv6()) {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn2Byte, sizeof(kSockOptOn2Byte)), -1)
        << strerror(errno);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn2Byte, sizeof(kSockOptOn2Byte)), 0)
        << strerror(errno);
  }

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  if (IsIPv6()) {
    EXPECT_EQ(get, kSockOptOff);
  } else {
    EXPECT_EQ(get, kSockOptOn);
  }

  if (IsIPv6()) {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff2Byte, sizeof(kSockOptOff2Byte)),
              -1)
        << strerror(errno);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff2Byte, sizeof(kSockOptOff2Byte)),
              0)
        << strerror(errno);
  }

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, UpdateAnyTimestampDisablesOtherTimestampOptions) {
  constexpr std::pair<SockOption, const char*> kOpts[] = {
      std::make_pair(GetTimestamp(), "SO_TIMESTAMP"),
      std::make_pair(GetTimestampNs(), "SO_TIMESTAMPNS"),
  };
  constexpr int optvals[] = {kSockOptOff, kSockOptOn};

  for (const auto& [opt_to_enable, opt_to_enable_name] : kOpts) {
    SCOPED_TRACE("Enable option " + std::string(opt_to_enable_name));
    for (const auto& [opt_to_update, opt_to_update_name] : kOpts) {
      SCOPED_TRACE("Update option " + std::string(opt_to_update_name));
      if (opt_to_enable == opt_to_update) {
        continue;
      }
      for (const int optval : optvals) {
        SCOPED_TRACE("Update value " + std::to_string(optval));
        fbl::unique_fd s;
        ASSERT_TRUE(s = NewSocket()) << strerror(errno);

        ASSERT_EQ(setsockopt(s.get(), opt_to_enable.level, opt_to_enable.option, &kSockOptOn,
                             sizeof(kSockOptOn)),
                  0)
            << strerror(errno);
        {
          int get = -1;
          socklen_t get_len = sizeof(get);
          ASSERT_EQ(getsockopt(s.get(), opt_to_enable.level, opt_to_enable.option, &get, &get_len),
                    0)
              << strerror(errno);
          EXPECT_EQ(get_len, sizeof(get));
          EXPECT_EQ(get, kSockOptOn);
        }

        ASSERT_EQ(
            setsockopt(s.get(), opt_to_update.level, opt_to_update.option, &optval, sizeof(optval)),
            0)
            << strerror(errno);
        {
          int get = -1;
          socklen_t get_len = sizeof(get);
          ASSERT_EQ(getsockopt(s.get(), opt_to_update.level, opt_to_update.option, &get, &get_len),
                    0)
              << strerror(errno);
          EXPECT_EQ(get_len, sizeof(get));
          EXPECT_EQ(get, optval);
        }

        // The initially enabled option should be disabled after the mutually exclusive option is
        // updated.
        {
          int get = -1;
          socklen_t get_len = sizeof(get);
          ASSERT_EQ(getsockopt(s.get(), opt_to_enable.level, opt_to_enable.option, &get, &get_len),
                    0)
              << strerror(errno);
          EXPECT_EQ(get_len, sizeof(get));
          EXPECT_EQ(get, kSockOptOff);
        }

        EXPECT_EQ(close(s.release()), 0) << strerror(errno);
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(LocalhostTest, SocketOptsTest,
                         testing::Combine(testing::Values(AF_INET, AF_INET6),
                                          testing::Values(SOCK_DGRAM, SOCK_STREAM)),
                         SocketKindToString);

using TypeMulticast = std::tuple<int, bool>;

std::string TypeMulticastToString(const testing::TestParamInfo<TypeMulticast>& info) {
  auto const& [type, multicast] = info.param;
  std::string addr;
  if (multicast) {
    addr = "Multicast";
  } else {
    addr = "Loopback";
  }
  return socketTypeToString(type) + addr;
}

class ReuseTest : public testing::TestWithParam<TypeMulticast> {};

TEST_P(ReuseTest, AllowsAddressReuse) {
  const int on = true;
  auto const& [type, multicast] = GetParam();

#if defined(__Fuchsia__)
  if (multicast && type == SOCK_STREAM) {
    GTEST_SKIP() << "Cannot bind a TCP socket to a multicast address on Fuchsia";
  }
#endif

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  if (multicast) {
    int n = inet_pton(addr.sin_family, "224.0.2.1", &addr.sin_addr);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
  }

  fbl::unique_fd s1;
  ASSERT_TRUE(s1 = fbl::unique_fd(socket(AF_INET, type, 0))) << strerror(errno);

// TODO(https://gvisor.dev/issue/3839): Remove this.
#if defined(__Fuchsia__)
  // Must outlive the block below.
  fbl::unique_fd s;
  if (type != SOCK_DGRAM && multicast) {
    ASSERT_EQ(bind(s1.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), -1);
    ASSERT_EQ(errno, EADDRNOTAVAIL) << strerror(errno);
    ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);
    ip_mreqn param = {
        .imr_multiaddr = addr.sin_addr,
        .imr_address =
            {
                .s_addr = htonl(INADDR_ANY),
            },
        .imr_ifindex = 1,
    };
    ASSERT_EQ(setsockopt(s.get(), SOL_IP, IP_ADD_MEMBERSHIP, &param, sizeof(param)), 0)
        << strerror(errno);
  }
#endif

  ASSERT_EQ(setsockopt(s1.get(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)), 0) << strerror(errno);
  ASSERT_EQ(bind(s1.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(s1.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  fbl::unique_fd s2;
  ASSERT_TRUE(s2 = fbl::unique_fd(socket(AF_INET, type, 0))) << strerror(errno);
  ASSERT_EQ(setsockopt(s2.get(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)), 0) << strerror(errno);
  ASSERT_EQ(bind(s2.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(LocalhostTest, ReuseTest,
                         testing::Combine(testing::Values(SOCK_DGRAM, SOCK_STREAM),
                                          testing::Values(false, true)),
                         TypeMulticastToString);

TEST(LocalhostTest, Accept) {
  fbl::unique_fd serverfd;
  ASSERT_TRUE(serverfd = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in6 serveraddr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  socklen_t serveraddrlen = sizeof(serveraddr);
  ASSERT_EQ(bind(serverfd.get(), reinterpret_cast<sockaddr*>(&serveraddr), serveraddrlen), 0)
      << strerror(errno);
  ASSERT_EQ(getsockname(serverfd.get(), reinterpret_cast<sockaddr*>(&serveraddr), &serveraddrlen),
            0)
      << strerror(errno);
  ASSERT_EQ(serveraddrlen, sizeof(serveraddr));
  ASSERT_EQ(listen(serverfd.get(), 0), 0) << strerror(errno);

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(clientfd.get(), reinterpret_cast<sockaddr*>(&serveraddr), serveraddrlen), 0)
      << strerror(errno);

  sockaddr_in connaddr;
  socklen_t connaddrlen = sizeof(connaddr);
  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(
                  accept(serverfd.get(), reinterpret_cast<sockaddr*>(&connaddr), &connaddrlen)))
      << strerror(errno);
  ASSERT_GT(connaddrlen, sizeof(connaddr));
}

TEST(LocalhostTest, AcceptAfterReset) {
  fbl::unique_fd server;
  ASSERT_TRUE(server = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(bind(server.get(), reinterpret_cast<const sockaddr*>(&addr), addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(getsockname(server.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));
  ASSERT_EQ(listen(server.get(), 0), 0) << strerror(errno);

  {
    fbl::unique_fd client;
    ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);
    ASSERT_EQ(connect(client.get(), reinterpret_cast<const sockaddr*>(&addr), addrlen), 0)
        << strerror(errno);
    linger opt = {
        .l_onoff = 1,
        .l_linger = 0,
    };
    ASSERT_EQ(setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
        << strerror(errno);

    // Ensure the accept queue has the passive connection enqueued before attempting to reset it.
    pollfd pfd = {
        .fd = server.get(),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLIN);

    // Close the client and trigger a RST.
    ASSERT_EQ(close(client.release()), 0) << strerror(errno);
  }

  fbl::unique_fd conn;
  ASSERT_TRUE(
      conn = fbl::unique_fd(accept(server.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen)))
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));
  ASSERT_EQ(addr.sin6_family, AF_INET6);
  char buf[INET6_ADDRSTRLEN];
  ASSERT_TRUE(IN6_IS_ADDR_LOOPBACK(&addr.sin6_addr))
      << inet_ntop(addr.sin6_family, &addr.sin6_addr, buf, sizeof(buf));
  ASSERT_NE(addr.sin6_port, 0);

  // Wait for the connection to close to avoid flakes when this code is reached before the RST
  // arrives at |conn|.
  {
    pollfd pfd = {
        .fd = conn.get(),
    };

    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLERR | POLLHUP);
  }

  int err;
  socklen_t optlen = sizeof(err);
  ASSERT_EQ(getsockopt(conn.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
  ASSERT_EQ(optlen, sizeof(err));
  ASSERT_EQ(err, ECONNRESET) << strerror(err);
}

TEST(LocalhostTest, ConnectAFMismatchINET) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_port = htons(1337),
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  EXPECT_EQ(connect(s.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), -1);
  EXPECT_EQ(errno, EAFNOSUPPORT) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, ConnectAFMismatchINET6) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(1337),
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  EXPECT_EQ(connect(s.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

// Test the behavior of poll on an unconnected or non-listening stream socket.
TEST(NetStreamTest, UnconnectPoll) {
  fbl::unique_fd init, bound;
  ASSERT_TRUE(init = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_TRUE(bound = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  ASSERT_EQ(bind(bound.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  constexpr short masks[] = {
      0,
      POLLIN | POLLOUT | POLLPRI | POLLRDHUP,
  };
  for (short events : masks) {
    pollfd pfds[] = {{
                         .fd = init.get(),
                         .events = events,
                     },
                     {
                         .fd = bound.get(),
                         .events = events,
                     }};
    int n = poll(pfds, std::size(pfds), std::chrono::milliseconds(kTimeout).count());
    EXPECT_GE(n, 0) << strerror(errno);
    EXPECT_EQ(n, static_cast<int>(std::size(pfds))) << " events = " << std::hex << events;

    for (size_t i = 0; i < std::size(pfds); i++) {
      EXPECT_EQ(pfds[i].revents, (events & POLLOUT) | POLLHUP) << i;
    }
  }

  // Poll on listening socket does timeout on no incoming connections.
  ASSERT_EQ(listen(bound.get(), 0), 0) << strerror(errno);
  pollfd pfd = {
      .fd = bound.get(),
  };
  EXPECT_EQ(poll(&pfd, 1, 0), 0) << strerror(errno);
}

TEST(NetStreamTest, ConnectTwice) {
  fbl::unique_fd client, listener;
  ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  ASSERT_EQ(connect(client.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), -1);
  ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);

  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  // TODO(https://fxbug.dev/61594): decide if we want to match Linux's behaviour.
  ASSERT_EQ(connect(client.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
#if defined(__linux__)
            0)
      << strerror(errno);
#else
            -1);
  ASSERT_EQ(errno, ECONNABORTED) << strerror(errno);
#endif

  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
  ASSERT_EQ(close(client.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, ConnectCloseRace) {
  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  // Use the ephemeral port allocated by the stack as destination address for connect.
  {
    fbl::unique_fd tmp;
    ASSERT_TRUE(tmp = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    ASSERT_EQ(bind(tmp.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
    socklen_t addrlen = sizeof(addr);
    ASSERT_EQ(getsockname(tmp.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
        << strerror(errno);
    ASSERT_EQ(addrlen, sizeof(addr));
  }

  std::array<std::thread, 50> threads;
  for (auto& t : threads) {
    t = std::thread([&] {
      for (int i = 0; i < 5; i++) {
        fbl::unique_fd client;
        ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
            << strerror(errno);

        ASSERT_EQ(connect(client.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
                  -1);
        ASSERT_TRUE(errno == EINPROGRESS
#if !defined(__Fuchsia__)
                    // Linux could return ECONNREFUSED if it processes the incoming RST before
                    // connect system
                    // call returns.
                    || errno == ECONNREFUSED
#endif
                    )
            << strerror(errno);
        ASSERT_EQ(close(client.release()), 0) << strerror(errno);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }
}

enum class CloseTarget {
  CLIENT,
  SERVER,
};

constexpr const char* CloseTargetToString(const CloseTarget s) {
  switch (s) {
    case CloseTarget::CLIENT:
      return "Client";
    case CloseTarget::SERVER:
      return "Server";
  }
}

enum class HangupMethod {
  kClose,
  kShutdown,
};

constexpr const char* HangupMethodToString(const HangupMethod s) {
  switch (s) {
    case HangupMethod::kClose:
      return "Close";
    case HangupMethod::kShutdown:
      return "Shutdown";
  }
}

void ExpectLastError(const fbl::unique_fd& fd, int expected) {
  int err;
  socklen_t optlen = sizeof(err);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
  ASSERT_EQ(optlen, sizeof(err));
  EXPECT_EQ(err, expected) << " err=" << strerror(err) << " expected=" << strerror(expected);
}

using HangupParams = std::tuple<CloseTarget, HangupMethod>;

class HangupTest : public testing::TestWithParam<HangupParams> {};

TEST_P(HangupTest, DuringConnect) {
  auto const& [close_target, hangup_method] = GetParam();

  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  sockaddr_in addr_in = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  auto* addr = reinterpret_cast<sockaddr*>(&addr_in);
  socklen_t addr_len = sizeof(addr_in);

  ASSERT_EQ(bind(listener.get(), addr, addr_len), 0) << strerror(errno);
  {
    socklen_t addr_len_in = addr_len;
    ASSERT_EQ(getsockname(listener.get(), addr, &addr_len), 0) << strerror(errno);
    EXPECT_EQ(addr_len, addr_len_in);
  }
  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  fbl::unique_fd established_client;
  ASSERT_TRUE(established_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0)))
      << strerror(errno);
  ASSERT_EQ(connect(established_client.get(), addr, addr_len), 0) << strerror(errno);

  // Ensure that the accept queue has the completed connection.
  {
    pollfd pfd = {
        .fd = listener.get(),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(pfd.revents, POLLIN);
  }

  // Connect asynchronously since this one will end up in SYN-SENT.
  fbl::unique_fd connecting_client;
  ASSERT_TRUE(connecting_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  EXPECT_EQ(connect(connecting_client.get(), addr, addr_len), -1);
  EXPECT_EQ(errno, EINPROGRESS) << strerror(errno);

  switch (close_target) {
    case CloseTarget::CLIENT:
      switch (hangup_method) {
        case HangupMethod::kClose: {
          ASSERT_EQ(close(established_client.release()), 0) << strerror(errno);
          // Closing the established client isn't enough; the connection must be accepted before
          // the connecting client can make progress.
          EXPECT_EQ(connect(connecting_client.get(), addr, addr_len), -1) << strerror(errno);
          EXPECT_EQ(errno, EALREADY) << strerror(errno);

          ASSERT_EQ(close(connecting_client.release()), 0) << strerror(errno);

          // Established connection is still in the accept queue, even though it's closed.
          fbl::unique_fd accepted;
          EXPECT_TRUE(accepted = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)))
              << strerror(errno);

          // Incomplete connection never made it into the queue.
          EXPECT_FALSE(accepted = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)));
          EXPECT_EQ(errno, EAGAIN) << strerror(errno);

          break;
        }
        case HangupMethod::kShutdown: {
          ASSERT_EQ(shutdown(connecting_client.get(), SHUT_RD), 0) << strerror(errno);

          {
            pollfd pfd = {
                .fd = connecting_client.get(),
                .events = std::numeric_limits<decltype(pfd.events)>::max(),
            };
#if !defined(__Fuchsia__)
            int n = poll(&pfd, 1, 0);
            EXPECT_GE(n, 0) << strerror(errno);
            EXPECT_EQ(n, 1);
            EXPECT_EQ(pfd.revents, POLLOUT | POLLWRNORM | POLLHUP | POLLERR);
#else
            // TODO(https://fxbug.dev/81448): Poll for POLLIN and POLLRDHUP to show their absence.
            // Can't be polled now because these events are asserted synchronously, and they might
            // be ready before the other expected events are asserted.
            pfd.events ^= (POLLIN | POLLRDHUP);
            // TODO(https://fxbug.dev/85279): Remove the poll timeout.
            int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
            EXPECT_GE(n, 0) << strerror(errno);
            EXPECT_EQ(n, 1);
            // TODO(https://fxbug.dev/73258): Add POLLWRNORM to the expectations.
            EXPECT_EQ(pfd.revents, POLLOUT | POLLHUP | POLLERR);
#endif
          }

          EXPECT_EQ(connect(connecting_client.get(), addr, addr_len), -1);
#if !defined(__Fuchsia__)
          EXPECT_EQ(errno, EINPROGRESS) << strerror(errno);
#else
          // TODO(https://fxbug.dev/61594): Fuchsia doesn't allow never-connected socket reuse.
          EXPECT_EQ(errno, ECONNRESET) << strerror(errno);
#endif
          // connect result was consumed by the connect call.
          ASSERT_NO_FATAL_FAILURE(ExpectLastError(connecting_client, 0));

          ASSERT_EQ(shutdown(established_client.get(), SHUT_RD), 0) << strerror(errno);

          {
            pollfd pfd = {
                .fd = established_client.get(),
                .events = POLLIN,
            };
            int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
            EXPECT_GE(n, 0) << strerror(errno);
            EXPECT_EQ(n, 1);
            EXPECT_EQ(pfd.revents, POLLIN);
          }

          EXPECT_EQ(connect(established_client.get(), addr, addr_len), -1);
          EXPECT_EQ(errno, EISCONN) << strerror(errno);
          ASSERT_NO_FATAL_FAILURE(ExpectLastError(established_client, 0));

          break;
        }
      }
      break;
    case CloseTarget::SERVER: {
      switch (hangup_method) {
        case HangupMethod::kClose:
          ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
          break;
        case HangupMethod::kShutdown: {
          ASSERT_EQ(shutdown(listener.get(), SHUT_RD), 0) << strerror(errno);
          pollfd pfd = {
              .fd = listener.get(),
              .events = std::numeric_limits<decltype(pfd.events)>::max(),
          };
#if !defined(__Fuchsia__)
          int n = poll(&pfd, 1, 0);
          EXPECT_GE(n, 0) << strerror(errno);
          EXPECT_EQ(n, 1);
          EXPECT_EQ(pfd.revents, POLLOUT | POLLWRNORM | POLLHUP);
#else
          // TODO(https://fxbug.dev/81448): Poll for POLLIN and POLLRDHUP to show their absence.
          // Can't be polled now because these events are asserted synchronously, and they might
          // be ready before the other expected events are asserted.
          pfd.events ^= (POLLIN | POLLRDHUP);
          // TODO(https://fxbug.dev/85279): Remove the poll timeout.
          int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
          EXPECT_GE(n, 0) << strerror(errno);
          EXPECT_EQ(n, 1);
          // TODO(https://fxbug.dev/85283): Remove POLLERR from the expectations.
          // TODO(https://fxbug.dev/73258): Add POLLWRNORM to the expectations.
          EXPECT_EQ(pfd.revents, POLLOUT | POLLHUP | POLLERR);
#endif
          break;
        }
      }

      const struct {
        const fbl::unique_fd& fd;
        const int connect_result;
        const int last_error;
      } expectations[] = {
          {
              .fd = established_client,
#if defined(__Fuchsia__)
              // We're doing the wrong thing here. Broadly what seems to be happening:
              // - closing the listener causes a RST to be sent
              // - when RST is received, the endpoint moves to an error state
              // - loop{Read,Write} observes the error and stores it in the terminal error
              // - tcpip.Endpoint.Connect returns ErrConnectionAborted
              //   - the terminal error is returned
              //
              // Linux seems to track connectedness separately from the TCP state machine state;
              // when an endpoint becomes connected, it never becomes unconnected with respect to
              // the behavior of `connect`.
              //
              // Since the call to tcpip.Endpoint.Connect does the wrong thing, this is likely a
              // gVisor bug.
              .connect_result = ECONNRESET,
              .last_error = 0,
#else
              .connect_result = EISCONN,
              .last_error = ECONNRESET,
#endif
          },
          {
              .fd = connecting_client,
              .connect_result = ECONNREFUSED,
              .last_error = 0,
          },
      };

      for (size_t i = 0; i < std::size(expectations); i++) {
        SCOPED_TRACE("i=" + std::to_string(i));

        const auto& expected = expectations[i];
        AssertExpectedReventsAfterPeerShutdown(expected.fd.get());
        EXPECT_EQ(connect(expected.fd.get(), addr, addr_len), -1);
        EXPECT_EQ(errno, expected.connect_result)
            << " errno=" << strerror(errno) << " expected=" << strerror(expected.connect_result);

        ASSERT_NO_FATAL_FAILURE(ExpectLastError(expected.fd, expected.last_error));
      }

      break;
    }
  }
}

std::string HangupParamsToString(const testing::TestParamInfo<HangupParams> info) {
  auto const& [close_target, hangup_method] = info.param;
  std::stringstream s;
  s << HangupMethodToString(hangup_method);
  s << CloseTargetToString(close_target);
  return s.str();
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, HangupTest,
                         testing::Combine(testing::Values(CloseTarget::CLIENT, CloseTarget::SERVER),
                                          testing::Values(HangupMethod::kClose,
                                                          HangupMethod::kShutdown)),
                         HangupParamsToString);

TEST(LocalhostTest, RaceLocalPeerClose) {
  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
#if !defined(__Fuchsia__)
  // Make the listener non-blocking so that we can let accept system call return
  // below when there are no acceptable connections.
  int flags;
  ASSERT_GE(flags = fcntl(listener.get(), F_GETFL, 0), 0) << strerror(errno);
  ASSERT_EQ(fcntl(listener.get(), F_SETFL, flags | O_NONBLOCK), 0) << strerror(errno);
#endif
  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  std::array<std::thread, 50> threads;
  ASSERT_EQ(listen(listener.get(), threads.size()), 0) << strerror(errno);

  // Run many iterations in parallel in order to increase load on Netstack and increase the
  // probability we'll hit the problem.
  for (auto& t : threads) {
    t =
        std::thread([&] {
          fbl::unique_fd peer;
          ASSERT_TRUE(peer = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

          // Connect and immediately close a peer with linger. This causes the network-initiated
          // close that will race with the accepted connection close below. Linger is necessary
          // because we need a TCP RST to force a full teardown, tickling Netstack the right way to
          // cause a bad race.
          linger opt = {
              .l_onoff = 1,
              .l_linger = 0,
          };
          EXPECT_EQ(setsockopt(peer.get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
              << strerror(errno);
          ASSERT_EQ(connect(peer.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
              << strerror(errno);
          ASSERT_EQ(close(peer.release()), 0) << strerror(errno);

          // Accept the connection and close it, adding new racing signal (operating on `close`) to
          // Netstack.
          auto local = fbl::unique_fd(accept(listener.get(), nullptr, nullptr));
          if (!local.is_valid()) {
#if !defined(__Fuchsia__)
            // We get EAGAIN when there are no pending acceptable connections. Though the peer
            // connect was a blocking call, it can return before the final ACK is sent out causing
            // the RST from linger0+close to be sent out before the final ACK. This would result in
            // that connection to be not completed and hence not added to the acceptable queue.
            //
            // The above race does not currently exist on Fuchsia where the final ACK would always
            // be sent out over lo before connect() call returns.
            ASSERT_EQ(errno, EAGAIN)
#else
            FAIL()
#endif
                << strerror(errno);
          } else {
            ASSERT_EQ(close(local.release()), 0) << strerror(errno);
          }
        });
  }

  for (auto& t : threads) {
    t.join();
  }

  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, GetAddrInfo) {
  addrinfo hints = {
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_STREAM,
  };

  addrinfo* result;
  ASSERT_EQ(getaddrinfo("localhost", nullptr, &hints, &result), 0) << strerror(errno);

  int i = 0;
  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    i++;

    EXPECT_EQ(ai->ai_socktype, hints.ai_socktype);
    const sockaddr* sa = ai->ai_addr;

    switch (ai->ai_family) {
      case AF_INET: {
        EXPECT_EQ(ai->ai_addrlen, (socklen_t)16);

        unsigned char expected_addr[4] = {0x7f, 0x00, 0x00, 0x01};

        auto sin = reinterpret_cast<const sockaddr_in*>(sa);
        EXPECT_EQ(sin->sin_addr.s_addr, *reinterpret_cast<uint32_t*>(expected_addr));

        break;
      }
      case AF_INET6: {
        EXPECT_EQ(ai->ai_addrlen, (socklen_t)28);

        const char expected_addr[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

        auto sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
        EXPECT_STREQ(reinterpret_cast<const char*>(sin6->sin6_addr.s6_addr), expected_addr);

        break;
      }
    }
  }
  EXPECT_EQ(i, 2);
  freeaddrinfo(result);
}

class NetStreamSocketsTest : public testing::Test {
 protected:
  void SetUp() override {
    fbl::unique_fd listener;
    ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr =
            {
                .s_addr = htonl(INADDR_ANY),
            },
    };
    ASSERT_EQ(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);

    socklen_t addrlen = sizeof(addr);
    ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
        << strerror(errno);
    ASSERT_EQ(addrlen, sizeof(addr));

    ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

    ASSERT_TRUE(client_ = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
    ASSERT_EQ(connect(client_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);

    ASSERT_TRUE(server_ = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)))
        << strerror(errno);
    ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
  }

  fbl::unique_fd& client() { return client_; }

  fbl::unique_fd& server() { return server_; }

 private:
  fbl::unique_fd client_;
  fbl::unique_fd server_;
};

TEST_F(NetStreamSocketsTest, PartialWriteStress) {
  // Generate a payload large enough to fill the client->server buffers.
  std::string big_string;
  {
    size_t tx_capacity;
    ASSERT_NO_FATAL_FAILURE(TxCapacity(client().get(), tx_capacity));

    size_t rx_capacity;
    ASSERT_NO_FATAL_FAILURE(RxCapacity(server().get(), rx_capacity));
    const size_t size = tx_capacity + rx_capacity;
    big_string.reserve(size);
    while (big_string.size() < size) {
      big_string += "Though this upload be but little, it is fierce.";
    }
  }

  {
    // Write in small chunks to allow the outbound TCP to coalesce adjacent writes into a single
    // segment; that is the circumstance in which the data corruption bug that prompted writing
    // this test was observed.
    //
    // Loopback MTU is 64KiB, so use a value smaller than that.
    constexpr size_t write_size = 1 << 10;  // 1 KiB.

    auto s = big_string;
    while (!s.empty()) {
      ssize_t w = write(client().get(), s.data(), std::min(s.size(), write_size));
      ASSERT_GE(w, 0) << strerror(errno);
      s = s.substr(w);
    }
    ASSERT_EQ(shutdown(client().get(), SHUT_WR), 0) << strerror(errno);
  }

  // Read the data and validate it against our payload.
  {
    // Read in small chunks to increase the probability of partial writes from the network
    // endpoint into the zircon socket; that is the circumstance in which the data corruption bug
    // that prompted writing this test was observed.
    //
    // zircon sockets are 256KiB deep, so use a value smaller than that.
    //
    // Note that in spite of the trickery we employ in this test to create the conditions
    // necessary to trigger the data corruption bug, it is still not guaranteed to happen. This is
    // because a race is still necessary to trigger the bug; as netstack is copying bytes from the
    // network to the zircon socket, the application on the other side of this socket (this test)
    // must read between a partial write and the next write.
    constexpr size_t read_size = 1 << 13;  // 8 KiB.

    std::string buf;
    buf.resize(read_size);
    for (size_t i = 0; i < big_string.size();) {
      ssize_t r = read(server().get(), buf.data(), buf.size());
      ASSERT_GT(r, 0) << strerror(errno);

      auto actual = buf.substr(0, r);
      auto expected = big_string.substr(i, r);

      constexpr size_t kChunkSize = 100;
      for (size_t j = 0; j < actual.size(); j += kChunkSize) {
        auto actual_chunk = actual.substr(j, kChunkSize);
        auto expected_chunk = expected.substr(j, actual_chunk.size());
        ASSERT_EQ(actual_chunk, expected_chunk) << "offset " << i + j;
      }
      i += r;
    }
  }
}

TEST_F(NetStreamSocketsTest, PeerClosedPOLLOUT) {
  ASSERT_NO_FATAL_FAILURE(fill_stream_send_buf(server().get(), client().get(), nullptr));

  EXPECT_EQ(close(client().release()), 0) << strerror(errno);

  pollfd pfd = {
      .fd = server().get(),
      .events = POLLOUT,
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  EXPECT_GE(n, 0) << strerror(errno);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLOUT | POLLERR | POLLHUP);
}

TEST_F(NetStreamSocketsTest, BlockingAcceptWrite) {
  const char msg[] = "hello";
  ASSERT_EQ(write(server().get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_EQ(close(server().release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(client().get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_STREQ(buf, msg);
}

TEST_F(NetStreamSocketsTest, SocketAtOOBMark) {
  int result = sockatmark(client().get());
#if defined(__Fuchsia__)
  // sockatmark is not supported on Fuchsia.
  EXPECT_EQ(result, -1);
  // TODO(https://fxbug.dev/84632): This should be ENOSYS, not ENOTTY.
  EXPECT_EQ(errno, ENOTTY) << strerror(errno);
#else   //  defined(__Fuchsia__)
  EXPECT_EQ(result, 0) << strerror(errno);
#endif  // defined(__Fuchsia__)
}

TEST_F(NetStreamSocketsTest, Sendmmsg) {
  mmsghdr header{
      .msg_hdr = {},
      .msg_len = 0,
  };
  int result = sendmmsg(client().get(), &header, 0u, 0u);
#if defined(__Fuchsia__)
  // Fuchsia does not support sendmmsg().
  // TODO(https://fxbug.dev/45262, https://fxbug.dev/42678): Implement sendmmsg().
  EXPECT_EQ(result, -1);
  EXPECT_EQ(errno, ENOSYS) << strerror(errno);
#else   // defined(__Fuchsia__)
  EXPECT_EQ(result, 0) << strerror(errno);
#endif  // defined(__Fuchsia__)
}

TEST_F(NetStreamSocketsTest, Recvmmsg) {
  mmsghdr header{
      .msg_hdr = {},
      .msg_len = 0,
  };
  int result = recvmmsg(client().get(), &header, 1u, MSG_DONTWAIT, nullptr);
  EXPECT_EQ(result, -1);
#if __Fuchsia__
  // Fuchsia does not support recvmmsg().
  // TODO(https://fxbug.dev/45260): Implement recvmmsg().
  EXPECT_EQ(errno, ENOSYS) << strerror(errno);
#else   // __Fuchsia__
  EXPECT_EQ(errno, EAGAIN) << strerror(errno);
#endif  // __Fuchsia__
}

class TimeoutSockoptsTest : public testing::TestWithParam<int /* optname */> {};

TEST_P(TimeoutSockoptsTest, TimeoutSockopts) {
  int optname = GetParam();
  ASSERT_TRUE(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO);

  fbl::unique_fd socket_fd;
  ASSERT_TRUE(socket_fd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  // Set the timeout.
  const timeval expected_tv = {
      .tv_sec = 39,
      // NB: for some reason, Linux's resolution is limited to 4ms.
      .tv_usec = 504000,
  };
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &expected_tv, sizeof(expected_tv)), 0)
      << strerror(errno);

  // Reading it back should work.
  {
    timeval actual_tv;
    socklen_t optlen = sizeof(actual_tv);
    EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen), 0)
        << strerror(errno);
    EXPECT_EQ(optlen, sizeof(actual_tv));
    EXPECT_EQ(actual_tv.tv_sec, expected_tv.tv_sec);
    EXPECT_EQ(actual_tv.tv_usec, expected_tv.tv_usec);
  }

  // Reading it back with too much space should work and set optlen.
  {
    struct {
      timeval tv;
      char unused;
    } actual_tv_with_extra = {
        .unused = 0x44,
    };
    socklen_t optlen = sizeof(actual_tv_with_extra);
    EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv_with_extra, &optlen), 0)
        << strerror(errno);
    EXPECT_EQ(optlen, sizeof(timeval));
    EXPECT_EQ(actual_tv_with_extra.tv.tv_sec, expected_tv.tv_sec);
    EXPECT_EQ(actual_tv_with_extra.tv.tv_usec, expected_tv.tv_usec);
    EXPECT_EQ(actual_tv_with_extra.unused, 0x44);
  }

  // Reading it back without enough space should fail gracefully.
  {
    constexpr char kGarbage = 0x44;
    timeval actual_tv;
    memset(&actual_tv, kGarbage, sizeof(actual_tv));
    constexpr socklen_t too_small = sizeof(actual_tv) - 7;
    static_assert(too_small > 0);
    socklen_t optlen = too_small;
    // TODO: Decide if we want to match Linux's behaviour. It writes to only
    // the first optlen bytes of the timeval.
    EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen),
#if defined(__Fuchsia__)
              -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
#else
              0)
        << strerror(errno);
    EXPECT_EQ(optlen, too_small);
    EXPECT_EQ(memcmp(&actual_tv, &expected_tv, too_small), 0);
    const char* tv = reinterpret_cast<char*>(&actual_tv);
    for (size_t i = too_small; i < sizeof(actual_tv); i++) {
      EXPECT_EQ(tv[i], kGarbage);
    }
#endif
  }

  // Setting it without enough space should fail gracefully.
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &expected_tv, sizeof(expected_tv) - 1),
            -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);

  // Setting it with too much space should work okay.
  {
    const timeval expected_tv2 = {
        .tv_sec = 42,
        .tv_usec = 0,
    };
    socklen_t optlen = sizeof(expected_tv2) + 1;  // Too big.
    EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &expected_tv2, optlen), 0)
        << strerror(errno);

    timeval actual_tv;
    EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen), 0)
        << strerror(errno);
    EXPECT_EQ(optlen, sizeof(expected_tv2));
    EXPECT_EQ(actual_tv.tv_sec, expected_tv2.tv_sec);
    EXPECT_EQ(actual_tv.tv_usec, expected_tv2.tv_usec);
  }

  // Disabling rcvtimeo by setting it to zero should work.
  const timeval zero_tv = {
      .tv_sec = 0,
      .tv_usec = 0,
  };
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &zero_tv, sizeof(zero_tv)), 0)
      << strerror(errno);

  // Reading back the disabled timeout should work.
  {
    timeval actual_tv;
    memset(&actual_tv, 55, sizeof(actual_tv));
    socklen_t optlen = sizeof(actual_tv);
    EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen), 0)
        << strerror(errno);
    EXPECT_EQ(optlen, sizeof(actual_tv));
    EXPECT_EQ(actual_tv.tv_sec, zero_tv.tv_sec);
    EXPECT_EQ(actual_tv.tv_usec, zero_tv.tv_usec);
  }
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, TimeoutSockoptsTest,
                         testing::Values(SO_RCVTIMEO, SO_SNDTIMEO));

const int32_t kConnections = 100;

TEST(NetStreamTest, BlockingAcceptWriteMultiple) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), kConnections), 0) << strerror(errno);

  fbl::unique_fd clientfds[kConnections];
  for (auto& clientfd : clientfds) {
    ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
    ASSERT_EQ(connect(clientfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  const char msg[] = "hello";
  for (int i = 0; i < kConnections; i++) {
    fbl::unique_fd connfd;
    ASSERT_TRUE(connfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

    ASSERT_EQ(write(connfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
    ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);
  }

  for (auto& clientfd : clientfds) {
    char buf[sizeof(msg) + 1] = {};
    ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
    ASSERT_STREQ(buf, msg);
    ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  }

  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST_F(NetStreamSocketsTest, BlockingAcceptDupWrite) {
  fbl::unique_fd dupfd;
  ASSERT_TRUE(dupfd = fbl::unique_fd(dup(server().get()))) << strerror(errno);
  ASSERT_EQ(close(server().release()), 0) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(dupfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_EQ(close(dupfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(client().get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_STREQ(buf, msg);
}

TEST(NetStreamTest, NonBlockingAcceptWrite) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 0), 0) << strerror(errno);

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(clientfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  pollfd pfd = {
      .fd = acptfd.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(connfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, NonBlockingAcceptDupWrite) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 0), 0) << strerror(errno);

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(clientfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  pollfd pfd = {
      .fd = acptfd.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

  fbl::unique_fd dupfd;
  ASSERT_TRUE(dupfd = fbl::unique_fd(dup(connfd.get()))) << strerror(errno);
  ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(dupfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_EQ(close(dupfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, NonBlockingConnectWrite) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 0), 0) << strerror(errno);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  int ret;
  EXPECT_EQ(ret = connect(connfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    pollfd pfd = {
        .fd = connfd.get(),
        .events = POLLOUT,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);

    int err;
    socklen_t optlen = sizeof(err);
    ASSERT_EQ(getsockopt(connfd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
    ASSERT_EQ(optlen, sizeof(err));
    ASSERT_EQ(err, 0) << strerror(err);
  }

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(connfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, NonBlockingConnectRead) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 0), 0) << strerror(errno);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  int ret;
  EXPECT_EQ(ret = connect(connfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    fbl::unique_fd clientfd;
    ASSERT_TRUE(clientfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr)))
        << strerror(errno);

    const char msg[] = "hello";
    ASSERT_EQ(write(clientfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
    ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);

    // Note: the success of connection can be detected with POLLOUT, but
    // we use POLLIN here to wait until some data is written by the peer.
    pollfd pfd = {
        .fd = connfd.get(),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);

    int err;
    socklen_t optlen = sizeof(err);
    ASSERT_EQ(getsockopt(connfd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
    ASSERT_EQ(optlen, sizeof(err));
    ASSERT_EQ(err, 0) << strerror(err);

    char buf[sizeof(msg) + 1] = {};
    ASSERT_EQ(read(connfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
    ASSERT_STREQ(buf, msg);
    ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);
    EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
  }
}

class AddrKind {
 public:
  enum class Kind {
    V4,
    V6,
    V4MAPPEDV6,
  };

  explicit AddrKind(Kind kind) : kind_(kind) {}
  Kind Kind() const { return kind_; }

  constexpr const char* AddrKindToString() const {
    switch (kind_) {
      case Kind::V4:
        return "V4";
      case Kind::V6:
        return "V6";
      case Kind::V4MAPPEDV6:
        return "V4MAPPEDV6";
    }
  }

 private:
  const enum Kind kind_;
};

template <int socktype>
class SocketTest : public testing::TestWithParam<AddrKind> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sock_ = fbl::unique_fd(socket(Domain(), socktype, 0))) << strerror(errno);
  }

  void TearDown() override { ASSERT_EQ(close(sock_.release()), 0) << strerror(errno); }

  const fbl::unique_fd& sock() { return sock_; }

  sa_family_t Domain() const {
    switch (GetParam().Kind()) {
      case AddrKind::Kind::V4:
        return AF_INET;
      case AddrKind::Kind::V6:
      case AddrKind::Kind::V4MAPPEDV6:
        return AF_INET6;
    }
  }

  socklen_t AddrLen() const {
    if (Domain() == AF_INET) {
      return sizeof(sockaddr_in);
    }
    return sizeof(sockaddr_in6);
  }

  virtual sockaddr_storage Address(uint16_t port) const = 0;

 private:
  fbl::unique_fd sock_;
};

template <int socktype>
class AnyAddrSocketTest : public SocketTest<socktype> {
 protected:
  sockaddr_storage Address(uint16_t port) const override {
    sockaddr_storage addr{
        .ss_family = this->Domain(),
    };

    switch (this->GetParam().Kind()) {
      case AddrKind::Kind::V4: {
        auto sin = reinterpret_cast<sockaddr_in*>(&addr);
        sin->sin_addr.s_addr = htonl(INADDR_ANY);
        sin->sin_port = port;
        return addr;
      }
      case AddrKind::Kind::V6: {
        auto sin6 = reinterpret_cast<sockaddr_in6*>(&addr);
        sin6->sin6_addr = IN6ADDR_ANY_INIT;
        sin6->sin6_port = port;
        return addr;
      }
      case AddrKind::Kind::V4MAPPEDV6: {
        auto sin6 = reinterpret_cast<sockaddr_in6*>(&addr);
        sin6->sin6_addr = IN6ADDR_ANY_INIT;
        sin6->sin6_addr.s6_addr[10] = 0xff;
        sin6->sin6_addr.s6_addr[11] = 0xff;
        sin6->sin6_port = port;
        return addr;
      }
    }
  }
};

using AnyAddrStreamSocketTest = AnyAddrSocketTest<SOCK_STREAM>;
using AnyAddrDatagramSocketTest = AnyAddrSocketTest<SOCK_DGRAM>;

TEST_P(AnyAddrStreamSocketTest, Connect) {
  sockaddr_storage any = Address(0);
  socklen_t addrlen = AddrLen();
  ASSERT_EQ(connect(sock().get(), reinterpret_cast<const sockaddr*>(&any), addrlen), -1);
  ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);

  // The error should have been consumed.
  int err;
  socklen_t optlen = sizeof(err);
  ASSERT_EQ(getsockopt(sock().get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
  ASSERT_EQ(optlen, sizeof(err));
  ASSERT_EQ(err, 0) << strerror(err);
}

TEST_P(AnyAddrDatagramSocketTest, Connect) {
  sockaddr_storage any = Address(0);
  socklen_t addrlen = AddrLen();
  EXPECT_EQ(connect(sock().get(), reinterpret_cast<const sockaddr*>(&any), addrlen), 0)
      << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, AnyAddrStreamSocketTest,
                         testing::Values(AddrKind::Kind::V4, AddrKind::Kind::V6,
                                         AddrKind::Kind::V4MAPPEDV6),
                         [](const auto info) { return info.param.AddrKindToString(); });
INSTANTIATE_TEST_SUITE_P(NetDatagramTest, AnyAddrDatagramSocketTest,
                         testing::Values(AddrKind::Kind::V4, AddrKind::Kind::V6,
                                         AddrKind::Kind::V4MAPPEDV6),
                         [](const auto info) { return info.param.AddrKindToString(); });

class IOMethod {
 public:
  enum class Op {
    READ,
    READV,
    RECV,
    RECVFROM,
    RECVMSG,
    WRITE,
    WRITEV,
    SEND,
    SENDTO,
    SENDMSG,
  };

  explicit IOMethod(Op op) : op_(op) {}
  Op Op() const { return op_; }

  ssize_t ExecuteIO(const int fd, char* const buf, const size_t len) const {
    // Vectorize the provided buffer into multiple differently-sized iovecs.
    std::vector<iovec> iov;
    {
      char* iov_start = buf;
      size_t len_remaining = len;
      while (len_remaining != 0) {
        const size_t next_len = (len_remaining + 1) / 2;
        iov.push_back({
            .iov_base = iov_start,
            .iov_len = next_len,
        });
        len_remaining -= next_len;
        if (iov_start != nullptr) {
          iov_start += next_len;
        }
      }

      std::uniform_int_distribution<size_t> distr(0, iov.size());
      int seed = testing::UnitTest::GetInstance()->random_seed();
      std::default_random_engine rd(seed);
      iov.insert(iov.begin() + distr(rd), {
                                              .iov_base = buf,
                                              .iov_len = 0,
                                          });
    }

    msghdr msg = {
        .msg_iov = iov.data(),
        // Linux defines `msg_iovlen` as size_t, out of compliance with
        // with POSIX, whereas Fuchsia defines it as int. Bridge the
        // divide using decltype.
        .msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iov.size()),
    };

    switch (op_) {
      case Op::READ:
        return read(fd, buf, len);
      case Op::READV:
        return readv(fd, iov.data(), static_cast<int>(iov.size()));
      case Op::RECV:
        return recv(fd, buf, len, 0);
      case Op::RECVFROM:
        return recvfrom(fd, buf, len, 0, nullptr, nullptr);
      case Op::RECVMSG:
        return recvmsg(fd, &msg, 0);
      case Op::WRITE:
        return write(fd, buf, len);
      case Op::WRITEV:
        return writev(fd, iov.data(), static_cast<int>(iov.size()));
      case Op::SEND:
        return send(fd, buf, len, 0);
      case Op::SENDTO:
        return sendto(fd, buf, len, 0, nullptr, 0);
      case Op::SENDMSG:
        return sendmsg(fd, &msg, 0);
    }
  }

  bool isWrite() const {
    switch (op_) {
      case Op::READ:
      case Op::READV:
      case Op::RECV:
      case Op::RECVFROM:
      case Op::RECVMSG:
        return false;
      case Op::WRITE:
      case Op::WRITEV:
      case Op::SEND:
      case Op::SENDTO:
      case Op::SENDMSG:
      default:
        return true;
    }
  }

  constexpr const char* IOMethodToString() const {
    switch (op_) {
      case Op::READ:
        return "Read";
      case Op::READV:
        return "Readv";
      case Op::RECV:
        return "Recv";
      case Op::RECVFROM:
        return "Recvfrom";
      case Op::RECVMSG:
        return "Recvmsg";
      case Op::WRITE:
        return "Write";
      case Op::WRITEV:
        return "Writev";
      case Op::SEND:
        return "Send";
      case Op::SENDTO:
        return "Sendto";
      case Op::SENDMSG:
        return "Sendmsg";
    }
  }

 private:
  const enum Op op_;
};

#if !defined(__Fuchsia__)
// DisableSigPipe is typically invoked on Linux, in cases where the caller
// expects to perform stream socket writes on an unconnected socket. In such
// cases, SIGPIPE is expected on Linux. This returns a fit::deferred_action object
// whose destructor would undo the signal masking performed here.
//
// send{,to,msg} support the MSG_NOSIGNAL flag to suppress this behaviour, but
// write and writev do not.
auto DisableSigPipe(bool is_write) {
  struct sigaction act = {};
  act.sa_handler = SIG_IGN;
  struct sigaction oldact;
  if (is_write) {
    EXPECT_EQ(sigaction(SIGPIPE, &act, &oldact), 0) << strerror(errno);
  }
  return fit::defer([=]() {
    if (is_write) {
      EXPECT_EQ(sigaction(SIGPIPE, &oldact, nullptr), 0) << strerror(errno);
    }
  });
}
#endif

class IOMethodTest : public testing::TestWithParam<IOMethod> {};

void DoNullPtrIO(const fbl::unique_fd& fd, const fbl::unique_fd& other, IOMethod io_method,
                 bool datagram) {
  // A version of ioMethod::ExecuteIO with special handling for vectorized operations: a 1-byte
  // buffer is prepended to the argument.
  auto ExecuteIO = [io_method](int fd, char* buf, size_t len) {
    char buffer[1];
    iovec iov[] = {
        {
            .iov_base = buffer,
            .iov_len = sizeof(buffer),
        },
        {
            .iov_base = buf,
            .iov_len = len,
        },
    };
    msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = std::size(iov),
    };

    switch (io_method.Op()) {
      case IOMethod::Op::READ:
      case IOMethod::Op::RECV:
      case IOMethod::Op::RECVFROM:
      case IOMethod::Op::WRITE:
      case IOMethod::Op::SEND:
      case IOMethod::Op::SENDTO:
        return io_method.ExecuteIO(fd, buf, len);
      case IOMethod::Op::READV:
        return readv(fd, iov, std::size(iov));
      case IOMethod::Op::RECVMSG:
        return recvmsg(fd, &msg, 0);
      case IOMethod::Op::WRITEV:
        return writev(fd, iov, std::size(iov));
      case IOMethod::Op::SENDMSG:
        return sendmsg(fd, &msg, 0);
    }
  };

  auto prepareForRead = [&](const char* buf, size_t len) {
    ASSERT_EQ(send(other.get(), buf, len, 0), ssize_t(len)) << strerror(errno);

    // Wait for the packet to arrive since we are nonblocking.
    pollfd pfd = {
        .fd = fd.get(),
        .events = POLLIN,
    };

    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLIN);
  };

  auto confirmWrite = [&]() {
    char buffer[1];
#if defined(__Fuchsia__)
    if (!datagram) {
      switch (io_method.Op()) {
        case IOMethod::Op::WRITE:
        case IOMethod::Op::SEND:
        case IOMethod::Op::SENDTO:
          break;
        case IOMethod::Op::WRITEV:
        case IOMethod::Op::SENDMSG: {
          // Fuchsia doesn't comply because zircon sockets do not implement atomic vector
          // operations, so these vector operations end up having sent the byte provided in the
          // ExecuteIO closure. See https://fxbug.dev/67928 for more details.
          //
          // Wait for the packet to arrive since we are nonblocking.
          pollfd pfd = {
              .fd = other.get(),
              .events = POLLIN,
          };
          int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
          ASSERT_GE(n, 0) << strerror(errno);
          ASSERT_EQ(n, 1);
          EXPECT_EQ(pfd.revents, POLLIN);
          EXPECT_EQ(recv(other.get(), buffer, sizeof(buffer), 0), 1) << strerror(errno);
          return;
        }
        default:
          FAIL() << "unexpected method " << io_method.IOMethodToString();
      }
    }
#endif
    // Nothing was sent. This is not obvious in the vectorized case.
    EXPECT_EQ(recv(other.get(), buffer, sizeof(buffer), 0), -1);
    EXPECT_EQ(errno, EAGAIN) << strerror(errno);
  };

  // Receive some data so we can attempt to read it below.
  if (!io_method.isWrite()) {
    char buffer[] = {0x74, 0x75};
    prepareForRead(buffer, sizeof(buffer));
  }

  [&]() {
#if defined(__Fuchsia__)
    if (!datagram) {
      switch (io_method.Op()) {
        case IOMethod::Op::READ:
        case IOMethod::Op::RECV:
        case IOMethod::Op::RECVFROM:
        case IOMethod::Op::WRITE:
        case IOMethod::Op::SEND:
        case IOMethod::Op::SENDTO:
          break;

        case IOMethod::Op::READV:
        case IOMethod::Op::RECVMSG:
        case IOMethod::Op::WRITEV:
        case IOMethod::Op::SENDMSG:
          // Fuchsia doesn't comply because zircon sockets do not implement atomic vector
          // operations, so these vector operations report success on the byte provided in the
          // ExecuteIO closure.
          EXPECT_EQ(ExecuteIO(fd.get(), nullptr, 1), 1) << strerror(errno);
          return;
      }
    }
#endif
    EXPECT_EQ(ExecuteIO(fd.get(), nullptr, 1), -1);
    EXPECT_EQ(errno, EFAULT) << strerror(errno);
  }();

  if (io_method.isWrite()) {
    confirmWrite();
  } else {
    char buffer[1];
    auto result = ExecuteIO(fd.get(), buffer, sizeof(buffer));
    if (datagram) {
      // The datagram was consumed in spite of the buffer being null.
      EXPECT_EQ(result, -1);
      EXPECT_EQ(errno, EAGAIN) << strerror(errno);
    } else {
      ssize_t space = sizeof(buffer);
      switch (io_method.Op()) {
        case IOMethod::Op::READV:
        case IOMethod::Op::RECVMSG:
#if defined(__Fuchsia__)
          // Fuchsia consumed one byte above.
#else
          // An additional byte of space was provided in the ExecuteIO closure.
          space += 1;
#endif
          [[fallthrough]];
        case IOMethod::Op::READ:
        case IOMethod::Op::RECV:
        case IOMethod::Op::RECVFROM:
          break;
        default:
          FAIL() << "unexpected method " << io_method.IOMethodToString();
      }
      EXPECT_EQ(result, space) << strerror(errno);
    }
  }

  // Do it again, but this time write less data so that vector operations can work normally.
  if (!io_method.isWrite()) {
    char buffer[] = {0x74};
    prepareForRead(buffer, sizeof(buffer));
  }

  switch (io_method.Op()) {
    case IOMethod::Op::WRITEV:
    case IOMethod::Op::SENDMSG:
#if defined(__Fuchsia__)
      if (!datagram) {
        // Fuchsia doesn't comply because zircon sockets do not implement atomic vector
        // operations, so these vector operations report success on the byte provided in the
        // ExecuteIO closure.
        EXPECT_EQ(ExecuteIO(fd.get(), nullptr, 1), 1) << strerror(errno);
        break;
      }
#endif
      [[fallthrough]];
    case IOMethod::Op::READ:
    case IOMethod::Op::RECV:
    case IOMethod::Op::RECVFROM:
    case IOMethod::Op::WRITE:
    case IOMethod::Op::SEND:
    case IOMethod::Op::SENDTO:
      EXPECT_EQ(ExecuteIO(fd.get(), nullptr, 1), -1);
      EXPECT_EQ(errno, EFAULT) << strerror(errno);
      break;
    case IOMethod::Op::READV:
    case IOMethod::Op::RECVMSG:
      // These vectorized operations never reach the faulty buffer, so they work normally.
      EXPECT_EQ(ExecuteIO(fd.get(), nullptr, 1), 1) << strerror(errno);
      break;
  }

  if (io_method.isWrite()) {
    confirmWrite();
  } else {
    char buffer[1];
    auto result = ExecuteIO(fd.get(), buffer, sizeof(buffer));
    if (datagram) {
      // The datagram was consumed in spite of the buffer being null.
      EXPECT_EQ(result, -1);
      EXPECT_EQ(errno, EAGAIN) << strerror(errno);
    } else {
      switch (io_method.Op()) {
        case IOMethod::Op::READ:
        case IOMethod::Op::RECV:
        case IOMethod::Op::RECVFROM:
          EXPECT_EQ(result, ssize_t(sizeof(buffer))) << strerror(errno);
          break;
        case IOMethod::Op::READV:
        case IOMethod::Op::RECVMSG:
          // The byte we sent was consumed in the ExecuteIO closure.
          EXPECT_EQ(result, -1);
          EXPECT_EQ(errno, EAGAIN) << strerror(errno);
          break;
        default:
          FAIL() << "unexpected method " << io_method.IOMethodToString();
      }
    }
  }
}

TEST_P(IOMethodTest, UnconnectedSocketIO) {
  fbl::unique_fd sockfd;
  ASSERT_TRUE(sockfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  IOMethod io_method = GetParam();
  char buffer[1];
  bool is_write = io_method.isWrite();
#if !defined(__Fuchsia__)
  auto undo = DisableSigPipe(is_write);
#endif
  ASSERT_EQ(io_method.ExecuteIO(sockfd.get(), buffer, sizeof(buffer)), -1);
  if (is_write) {
    ASSERT_EQ(errno, EPIPE) << strerror(errno);
  } else {
    ASSERT_EQ(errno, ENOTCONN) << strerror(errno);
  }
}

TEST_P(IOMethodTest, ListenerSocketIO) {
  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in serveraddr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(listener.get(), reinterpret_cast<sockaddr*>(&serveraddr), sizeof(serveraddr)), 0)
      << strerror(errno);
  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  IOMethod io_method = GetParam();
  char buffer[1];
  bool is_write = io_method.isWrite();
#if !defined(__Fuchsia__)
  auto undo = DisableSigPipe(is_write);
#endif
  ASSERT_EQ(io_method.ExecuteIO(listener.get(), buffer, sizeof(buffer)), -1);
  if (is_write) {
    ASSERT_EQ(errno, EPIPE) << strerror(errno);
  } else {
    ASSERT_EQ(errno, ENOTCONN) << strerror(errno);
  }
}

TEST_P(IOMethodTest, NullptrFaultDGRAM) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  const sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = 1235,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  ASSERT_EQ(connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  DoNullPtrIO(fd, fd, GetParam(), true);
}

TEST_P(IOMethodTest, NullptrFaultSTREAM) {
  fbl::unique_fd listener, client, server;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  int ret;
  EXPECT_EQ(ret = connect(client.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    pollfd pfd = {
        .fd = client.get(),
        .events = POLLOUT,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
  }

  ASSERT_TRUE(server = fbl::unique_fd(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK)))
      << strerror(errno);
  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);

  DoNullPtrIO(client, server, GetParam(), false);
}

INSTANTIATE_TEST_SUITE_P(IOMethodTests, IOMethodTest,
                         testing::Values(IOMethod::Op::READ, IOMethod::Op::READV,
                                         IOMethod::Op::RECV, IOMethod::Op::RECVFROM,
                                         IOMethod::Op::RECVMSG, IOMethod::Op::WRITE,
                                         IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                         IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                         [](const auto info) { return info.param.IOMethodToString(); });

class IOReadingMethodTest : public testing::TestWithParam<IOMethod> {};

TEST_P(IOReadingMethodTest, DatagramSocketErrorWhileBlocked) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  {
    // Connect to an existing remote but on a port that is not being used.
    sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(1337),
        .sin_addr =
            {
                .s_addr = htonl(INADDR_LOOPBACK),
            },
    };
    ASSERT_EQ(connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  std::latch fut_started(1);
  const auto fut = std::async(std::launch::async, [&, read_method = GetParam()]() {
    fut_started.count_down();

    char bytes[1];
    // Block while waiting for data to be received.
    ASSERT_EQ(read_method.ExecuteIO(fd.get(), bytes, sizeof(bytes)), -1);
    ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);
  });
  fut_started.wait();
  ASSERT_NO_FATAL_FAILURE(AssertBlocked(fut));

  {
    // Precondition sanity check: no pending events on the socket.
    pollfd pfd = {
        .fd = fd.get(),
    };
    int n = poll(&pfd, 1, 0);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 0);
  }

  char bytes[1];
  // Send a UDP packet to trigger a port unreachable response.
  ASSERT_EQ(send(fd.get(), bytes, sizeof(bytes), 0), ssize_t(sizeof(bytes))) << strerror(errno);
  // The blocking recv call should terminate with an error.
  ASSERT_EQ(fut.wait_for(kTimeout), std::future_status::ready);

  {
    // Postcondition sanity check: no pending events on the socket, the POLLERR should've been
    // cleared by the read_method call.
    pollfd pfd = {
        .fd = fd.get(),
    };
    int n = poll(&pfd, 1, 0);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 0);
  }

  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(IOReadingMethodTests, IOReadingMethodTest,
                         testing::Values(IOMethod::Op::READ, IOMethod::Op::READV,
                                         IOMethod::Op::RECV, IOMethod::Op::RECVFROM,
                                         IOMethod::Op::RECVMSG),
                         [](const testing::TestParamInfo<IOMethod>& info) {
                           return info.param.IOMethodToString();
                         });

template <typename F>
void TestDatagramSocketClearPoller(bool nonblocking, F consumeError) {
  int flags = 0;
  if (nonblocking) {
    flags |= SOCK_NONBLOCK;
  }

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM | flags, 0))) << strerror(errno);

  {
    // Connect to an existing remote but on a port that is not being used.
    sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(1337),
        .sin_addr =
            {
                .s_addr = htonl(INADDR_LOOPBACK),
            },
    };

    ASSERT_EQ(connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  {
    // Precondition sanity check: no pending events on the socket.
    pollfd pfd = {
        .fd = fd.get(),
    };
    int n = poll(&pfd, 1, 0);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 0);
  }

  {
    // Send a UDP packet to trigger a port unreachable response.
    char bytes[1];
    ASSERT_EQ(send(fd.get(), bytes, sizeof(bytes), 0), ssize_t(sizeof(bytes))) << strerror(errno);
  }

  {
    // Expect a POLLERR to be signaled on the socket.
    pollfd pfd = {
        .fd = fd.get(),
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(pfd.revents & POLLERR, POLLERR);
  }

  consumeError(fd);

  {
    pollfd pfd = {
        .fd = fd.get(),
    };
    int n = poll(&pfd, 1, 0);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 0);
  }

  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

std::string nonBlockingToString(bool nonblocking) {
  if (nonblocking) {
    return "NonBlocking";
  }
  return "Blocking";
}

class NonBlockingOptionTest : public testing::TestWithParam<bool> {};

TEST_P(NonBlockingOptionTest, DatagramSocketClearErrorWithGetSockOpt) {
  bool nonblocking = GetParam();

  TestDatagramSocketClearPoller(nonblocking, [&](const fbl::unique_fd& fd) {
    ASSERT_NO_FATAL_FAILURE(ExpectLastError(fd, ECONNREFUSED));
  });
}

INSTANTIATE_TEST_SUITE_P(NetDatagramTest, NonBlockingOptionTest, testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                           return nonBlockingToString(info.param);
                         });

using NonBlockingOptionIOParams = std::tuple<IOMethod, bool>;

class NonBlockingOptionIOTest : public testing::TestWithParam<NonBlockingOptionIOParams> {};

TEST_P(NonBlockingOptionIOTest, DatagramSocketClearErrorWithIO) {
  auto const& [io_method, nonblocking] = GetParam();

  TestDatagramSocketClearPoller(nonblocking, [op = io_method](const fbl::unique_fd& fd) {
    char bytes[1];
    EXPECT_EQ(op.ExecuteIO(fd.get(), bytes, sizeof(bytes)), -1);
    EXPECT_EQ(errno, ECONNREFUSED) << strerror(errno);
  });
}

std::string NonBlockingOptionIOParamsToString(
    const testing::TestParamInfo<NonBlockingOptionIOParams> info) {
  auto const& [io_method, nonblocking] = info.param;
  std::stringstream s;
  s << nonBlockingToString(nonblocking);
  s << io_method.IOMethodToString();
  return s.str();
}

INSTANTIATE_TEST_SUITE_P(
    NetDatagramTest, NonBlockingOptionIOTest,
    testing::Combine(testing::Values(IOMethod::Op::READ, IOMethod::Op::READV, IOMethod::Op::RECV,
                                     IOMethod::Op::RECVFROM, IOMethod::Op::RECVMSG,
                                     IOMethod::Op::WRITE, IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                     IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                     testing::Values(false, true)),
    NonBlockingOptionIOParamsToString);

using ConnectingIOParams = std::tuple<IOMethod, bool>;

class ConnectingIOTest : public testing::TestWithParam<ConnectingIOParams> {};

// Tests the application behavior when we start to read and write from a stream socket that is not
// yet connected.
TEST_P(ConnectingIOTest, BlockedIO) {
  auto const& [io_method, close_listener] = GetParam();
  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  // Setup a test client connection over which we test socket reads
  // when the connection is not yet established.

  // Linux default behavior is to complete one more connection than what
  // was passed as listen backlog (zero here).
  // Hence we initiate 2 client connections in this order:
  // (1) a precursor client for the sole purpose of filling up the server
  //     accept queue after handshake completion.
  // (2) a test client that keeps trying to establish connection with
  //     server, but remains in SYN-SENT.
  fbl::unique_fd precursor_client;
  ASSERT_TRUE(precursor_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0)))
      << strerror(errno);
  ASSERT_EQ(connect(precursor_client.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  // Observe the precursor client connection on the server side. This ensures that the TCP stack's
  // server accept queue is updated with the precursor client connection before any subsequent
  // client connect requests. The precursor client connect call returns after handshake
  // completion, but not necessarily after the server side has processed the ACK from the client
  // and updated its accept queue.
  {
    pollfd pfd = {
        .fd = listener.get(),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(pfd.revents, POLLIN);
  }

  // The test client connection would get established _only_ after both
  // these conditions are met:
  // (1) prior client connections are accepted by the server thus
  //     making room for a new connection.
  // (2) the server-side TCP stack completes handshake in response to
  //     the retransmitted SYN for the test client connection.
  //
  // The test would likely perform socket reads before any connection
  // timeout.
  fbl::unique_fd test_client;
  ASSERT_TRUE(test_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  // Sample data to be written.
  char sample_data[] = "Sample Data";
  // To correctly test reads, keep alteast one byte larger read buffer than what would be written.
  char recvbuf[sizeof(sample_data) + 1] = {};
  bool is_write = io_method.isWrite();
  auto ExecuteIO = [&, op = io_method]() {
    if (is_write) {
      return op.ExecuteIO(test_client.get(), sample_data, sizeof(sample_data));
    }
    return op.ExecuteIO(test_client.get(), recvbuf, sizeof(recvbuf));
  };
#if !defined(__Fuchsia__)
  auto undo = DisableSigPipe(is_write);
#endif

  EXPECT_EQ(ExecuteIO(), -1);
  if (is_write) {
    EXPECT_EQ(errno, EPIPE) << strerror(errno);
  } else {
    EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  }

  ASSERT_EQ(connect(test_client.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), -1);
  ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

  // Test socket I/O without waiting for connection to be established.
  EXPECT_EQ(ExecuteIO(), -1);
  EXPECT_EQ(errno, EWOULDBLOCK) << strerror(errno);

  std::latch fut_started(1);
  // Asynchronously block on I/O from the test client socket.
  const auto fut = std::async(std::launch::async, [&, err = close_listener]() {
    // Make the socket blocking.
    int flags;
    EXPECT_GE(flags = fcntl(test_client.get(), F_GETFL, 0), 0) << strerror(errno);
    EXPECT_EQ(flags & O_NONBLOCK, O_NONBLOCK);
    EXPECT_EQ(fcntl(test_client.get(), F_SETFL, flags ^ O_NONBLOCK), 0) << strerror(errno);

    fut_started.count_down();

    if (err) {
      EXPECT_EQ(ExecuteIO(), -1);
      EXPECT_EQ(errno, ECONNREFUSED) << strerror(errno);
    } else {
      EXPECT_EQ(ExecuteIO(), ssize_t(sizeof(sample_data))) << strerror(errno);
    }
  });
  fut_started.wait();
  ASSERT_NO_FATAL_FAILURE(AssertBlocked(fut));

  if (close_listener) {
    ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
  } else {
    // Accept the precursor connection to make room for the test client
    // connection to complete.
    fbl::unique_fd precursor_accept;
    ASSERT_TRUE(precursor_accept = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)))
        << strerror(errno);
    ASSERT_EQ(close(precursor_accept.release()), 0) << strerror(errno);
    ASSERT_EQ(close(precursor_client.release()), 0) << strerror(errno);

    // Accept the test client connection.
    fbl::unique_fd test_accept;
    ASSERT_TRUE(test_accept =
                    fbl::unique_fd(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK)))
        << strerror(errno);

    if (is_write) {
      // Ensure that we read the data whose send request was enqueued until
      // the connection was established.

      // TODO(https://fxbug.dev/67928): Replace these multiple non-blocking
      // reads with a single blocking read after Fuchsia supports atomic
      // vectorized writes.
      size_t total = 0;
      while (total < sizeof(sample_data)) {
        pollfd pfd = {
            .fd = test_accept.get(),
            .events = POLLIN,
        };
        int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
        ASSERT_GE(n, 0) << strerror(errno);
        ASSERT_EQ(n, 1);
        ASSERT_EQ(pfd.revents, POLLIN);
        ssize_t res = read(test_accept.get(), recvbuf + total, sizeof(recvbuf) - total);
        ASSERT_GE(res, 0) << strerror(errno);
        total += res;
      }
      ASSERT_EQ(total, sizeof(sample_data));
      ASSERT_STREQ(recvbuf, sample_data);
    } else {
      // Write data to unblock the socket read on the test client connection.
      ASSERT_EQ(write(test_accept.get(), sample_data, sizeof(sample_data)),
                ssize_t(sizeof(sample_data)))
          << strerror(errno);
    }
  }

  EXPECT_EQ(fut.wait_for(kTimeout), std::future_status::ready);
}

std::string ConnectingIOParamsToString(const testing::TestParamInfo<ConnectingIOParams> info) {
  auto const& [io_method, close_listener] = info.param;
  std::stringstream s;
  if (close_listener) {
    s << "CloseListener";
  } else {
    s << "Accept";
  }
  s << "During" << io_method.IOMethodToString();

  return s.str();
}

INSTANTIATE_TEST_SUITE_P(
    NetStreamTest, ConnectingIOTest,
    testing::Combine(testing::Values(IOMethod::Op::READ, IOMethod::Op::READV, IOMethod::Op::RECV,
                                     IOMethod::Op::RECVFROM, IOMethod::Op::RECVMSG,
                                     IOMethod::Op::WRITE, IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                     IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                     testing::Values(false, true)),
    ConnectingIOParamsToString);

// Test close/shutdown of listening socket with multiple non-blocking connects.
// This tests client sockets in connected and connecting states.
void TestListenWhileConnect(const IOMethod& io_method, void (*stopListen)(fbl::unique_fd&)) {
  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
  // This test is only interested in deterministically getting a socket in
  // connecting state. For that, we use a listen backlog of zero which would
  // mean there is exactly one connection that gets established and is enqueued
  // to the accept queue. We poll on the listener to ensure that is enqueued.
  // After that the subsequent client connect will stay in connecting state as
  // the accept queue is full.
  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  fbl::unique_fd established_client;
  ASSERT_TRUE(established_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0)))
      << strerror(errno);
  ASSERT_EQ(connect(established_client.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  // Ensure that the accept queue has the completed connection.
  {
    pollfd pfd = {
        .fd = listener.get(),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(pfd.revents, POLLIN);
  }

  fbl::unique_fd connecting_client;
  ASSERT_TRUE(connecting_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  EXPECT_EQ(connect(connecting_client.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), -1);
  EXPECT_EQ(errno, EINPROGRESS) << strerror(errno);

  ASSERT_NO_FATAL_FAILURE(stopListen(listener));

  std::array<std::pair<int, int>, 2> sockets = {
      std::make_pair(established_client.get(), ECONNRESET),
      std::make_pair(connecting_client.get(), ECONNREFUSED),
  };
  for (size_t i = 0; i < sockets.size(); i++) {
    SCOPED_TRACE("i=" + std::to_string(i));
    auto [fd, expected_errno] = sockets[i];
    AssertExpectedReventsAfterPeerShutdown(fd);

    char c;
    EXPECT_EQ(io_method.ExecuteIO(fd, &c, sizeof(c)), -1);
    EXPECT_EQ(errno, expected_errno) << strerror(errno) << " vs " << strerror(expected_errno);

    {
      // The error should have been consumed.
      int err;
      socklen_t optlen = sizeof(err);
      ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
      ASSERT_EQ(optlen, sizeof(err));
      ASSERT_EQ(err, 0) << strerror(err);
    }

    bool is_write = io_method.isWrite();
#if !defined(__Fuchsia__)
    auto undo = DisableSigPipe(is_write);
#endif

    if (is_write) {
      ASSERT_EQ(io_method.ExecuteIO(fd, &c, sizeof(c)), -1);
      EXPECT_EQ(errno, EPIPE) << strerror(errno);

      // The error should have been consumed.
      int err;
      socklen_t optlen = sizeof(err);
      ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
      ASSERT_EQ(optlen, sizeof(err));
      ASSERT_EQ(err, 0) << strerror(err);
    } else {
      ASSERT_EQ(io_method.ExecuteIO(fd, &c, sizeof(c)), 0) << strerror(errno);
    }
  }
}

class StopListenWhileConnect : public testing::TestWithParam<IOMethod> {};

TEST_P(StopListenWhileConnect, Close) {
  TestListenWhileConnect(
      GetParam(), [](fbl::unique_fd& f) { ASSERT_EQ(close(f.release()), 0) << strerror(errno); });
}

TEST_P(StopListenWhileConnect, Shutdown) {
  TestListenWhileConnect(GetParam(), [](fbl::unique_fd& f) {
    ASSERT_EQ(shutdown(f.get(), SHUT_RD), 0) << strerror(errno);
  });
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, StopListenWhileConnect,
                         testing::Values(IOMethod::Op::READ, IOMethod::Op::READV,
                                         IOMethod::Op::RECV, IOMethod::Op::RECVFROM,
                                         IOMethod::Op::RECVMSG, IOMethod::Op::WRITE,
                                         IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                         IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                         [](const testing::TestParamInfo<IOMethod>& info) {
                           return info.param.IOMethodToString();
                         });

TEST(NetStreamTest, NonBlockingConnectRefused) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_ANY),
          },
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  // No listen() on acptfd.

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  int ret;
  EXPECT_EQ(ret = connect(connfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    pollfd pfd = {
        .fd = connfd.get(),
        .events = POLLOUT,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);

    int err;
    socklen_t optlen = sizeof(err);
    ASSERT_EQ(getsockopt(connfd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
    ASSERT_EQ(optlen, sizeof(err));
    ASSERT_EQ(err, ECONNREFUSED) << strerror(err);
  }

  EXPECT_EQ(close(connfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, GetTcpInfo) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  {
    tcp_info info;
    socklen_t info_len = sizeof(tcp_info);
    ASSERT_EQ(getsockopt(fd.get(), SOL_TCP, TCP_INFO, &info, &info_len), 0) << strerror(errno);
    ASSERT_EQ(sizeof(tcp_info), info_len);

#if defined(__Fuchsia__)
    // Unsupported fields are intentionally initialized with garbage for explicitness.
    constexpr int kGarbage = 0xff;
    uint32_t initialization;
    memset(&initialization, kGarbage, sizeof(initialization));

    ASSERT_NE(info.tcpi_state, initialization);
    ASSERT_NE(info.tcpi_ca_state, initialization);
    ASSERT_NE(info.tcpi_rto, initialization);
    ASSERT_NE(info.tcpi_rtt, initialization);
    ASSERT_NE(info.tcpi_rttvar, initialization);
    ASSERT_NE(info.tcpi_snd_ssthresh, initialization);
    ASSERT_NE(info.tcpi_snd_cwnd, initialization);
    ASSERT_NE(info.tcpi_reord_seen, initialization);

    tcp_info expected;
    memset(&expected, kGarbage, sizeof(expected));
    expected.tcpi_state = info.tcpi_state;
    expected.tcpi_ca_state = info.tcpi_ca_state;
    expected.tcpi_rto = info.tcpi_rto;
    expected.tcpi_rtt = info.tcpi_rtt;
    expected.tcpi_rttvar = info.tcpi_rttvar;
    expected.tcpi_snd_ssthresh = info.tcpi_snd_ssthresh;
    expected.tcpi_snd_cwnd = info.tcpi_snd_cwnd;
    expected.tcpi_reord_seen = info.tcpi_reord_seen;

    ASSERT_EQ(memcmp(&info, &expected, sizeof(tcp_info)), 0);
#endif
  }

  // Test that we can partially retrieve TCP_INFO.
  {
    uint8_t tcpi_state;
    socklen_t info_len = sizeof(tcpi_state);
    ASSERT_EQ(getsockopt(fd.get(), SOL_TCP, TCP_INFO, &tcpi_state, &info_len), 0)
        << strerror(errno);
    ASSERT_EQ(info_len, sizeof(tcpi_state));
  }

  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, GetSocketAcceptConn) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  auto assert_so_accept_conn_eq = [&fd](int expected) {
    int got = ~expected;
    socklen_t got_len = sizeof(got);
    ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len), 0)
        << strerror(errno);
    ASSERT_EQ(got_len, sizeof(got));
    ASSERT_EQ(got, expected);
  };

  {
    SCOPED_TRACE("initial");
    ASSERT_NO_FATAL_FAILURE(assert_so_accept_conn_eq(0));
  }

  {
    const sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr =
            {
                .s_addr = htonl(INADDR_ANY),
            },
    };
    ASSERT_EQ(bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  {
    SCOPED_TRACE("bound");
    ASSERT_NO_FATAL_FAILURE(assert_so_accept_conn_eq(0));
  }

  ASSERT_EQ(listen(fd.get(), 0), 0) << strerror(errno);

  {
    SCOPED_TRACE("listening");
    ASSERT_NO_FATAL_FAILURE(assert_so_accept_conn_eq(1));
  }

  ASSERT_EQ(shutdown(fd.get(), SHUT_WR), 0) << strerror(errno);

  {
    SCOPED_TRACE("shutdown-write");
    ASSERT_NO_FATAL_FAILURE(assert_so_accept_conn_eq(1));
  }

  ASSERT_EQ(shutdown(fd.get(), SHUT_RD), 0) << strerror(errno);

  // TODO(https://fxbug.dev/61714): Shutting down a listening endpoint is asynchronous in gVisor;
  // transitioning out of the listening state is the responsibility of
  // tcp.endpoint.protocolListenLoop
  // (https://cs.opensource.google/gvisor/gvisor/+/master:pkg/tcpip/transport/tcp/accept.go;l=742-762;drc=58b9bdfc21e792c5d529ec9f4ab0b2f2cd1ee082),
  // which is merely notified when tcp.endpoint.shutdown is called
  // (https://cs.opensource.google/gvisor/gvisor/+/master:pkg/tcpip/transport/tcp/endpoint.go;l=2493;drc=58b9bdfc21e792c5d529ec9f4ab0b2f2cd1ee082).
#if !defined(__Fuchsia__)
  {
    SCOPED_TRACE("shutdown-read");
    ASSERT_NO_FATAL_FAILURE(assert_so_accept_conn_eq(0));
  }
#endif
}

// Test socket reads on disconnected stream sockets.
TEST(NetStreamTest, DisconnectedRead) {
  fbl::unique_fd socketfd;
  ASSERT_TRUE(socketfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  timeval tv = {
      // Use minimal non-zero timeout as we expect the blocking recv to return before it actually
      // starts reading. Without the timeout, the test could deadlock on a blocking recv, when the
      // underlying code is broken.
      .tv_usec = 1u,
  };
  EXPECT_EQ(setsockopt(socketfd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0)
      << strerror(errno);
  // Test blocking socket read.
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, 0, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  // Test with MSG_PEEK.
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, MSG_PEEK, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);

  // Test non blocking socket read.
  int flags;
  EXPECT_GE(flags = fcntl(socketfd.get(), F_GETFL, 0), 0) << strerror(errno);
  EXPECT_EQ(fcntl(socketfd.get(), F_SETFL, flags | O_NONBLOCK), 0) << strerror(errno);
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, 0, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  // Test with MSG_PEEK.
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, MSG_PEEK, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  EXPECT_EQ(close(socketfd.release()), 0) << strerror(errno);
}

TEST_F(NetStreamSocketsTest, Shutdown) {
  EXPECT_EQ(shutdown(server().get(), SHUT_WR), 0) << strerror(errno);

  pollfd pfd = {
      .fd = client().get(),
      .events = POLLRDHUP,
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  EXPECT_GE(n, 0) << strerror(errno);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLRDHUP);
}

TEST_F(NetStreamSocketsTest, ResetOnFullReceiveBufferShutdown) {
  // Fill the receive buffer of the client socket.
  ASSERT_NO_FATAL_FAILURE(fill_stream_send_buf(server().get(), client().get(), nullptr));

  // Setting SO_LINGER to 0 and `close`ing the server socket should
  // immediately send a TCP RST.
  linger opt = {
      .l_onoff = 1,
      .l_linger = 0,
  };
  EXPECT_EQ(setsockopt(server().get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
      << strerror(errno);

  // Close the server to trigger a TCP RST now that linger is 0.
  EXPECT_EQ(close(server().release()), 0) << strerror(errno);

  // Wait for the RST.
  pollfd pfd = {
      .fd = client().get(),
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLHUP | POLLERR);

  // The socket is no longer connected.
  EXPECT_EQ(shutdown(client().get(), SHUT_RD), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);

  // Create another socket to ensure that the networking stack hasn't panicked.
  fbl::unique_fd test_sock;
  ASSERT_TRUE(test_sock = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
}

// Tests that a socket which has completed SHUT_RDWR responds to incoming data with RST.
TEST_F(NetStreamSocketsTest, ShutdownReset) {
  // This test is tricky. In Linux we could shutdown(SHUT_RDWR) the server socket, write() some
  // data on the client socket, and observe the server reply with RST. The SHUT_WR would move the
  // server socket state out of ESTABLISHED (to FIN-WAIT2 after sending FIN and receiving an ACK)
  // and SHUT_RD would close the receiver. Only when the server socket has transitioned out of
  // ESTABLISHED state. At this point, the server socket would respond to incoming data with RST.
  //
  // In Fuchsia this is more complicated because each socket is a distributed system (consisting
  // of netstack and fdio) wherein the socket state is eventually consistent. We must take care to
  // synchronize our actions with netstack's state as we're testing that netstack correctly sends
  // a RST in response to data received after shutdown(SHUT_RDWR).
  //
  // We can manipulate and inspect state using only shutdown() and poll(), both of which operate
  // on fdio state rather than netstack state. Combined with the fact that SHUT_RD is not
  // observable by the peer (i.e. doesn't cause any network traffic), means we are in a pickle.
  //
  // On the other hand, SHUT_WR does cause a FIN to be sent, which can be observed by the peer
  // using poll(POLLRDHUP). Note also that netstack observes SHUT_RD and SHUT_WR on different
  // threads, meaning that a race condition still exists. At the time of writing, this is the best
  // we can do.

  // Change internal state to disallow further reads and writes. The state change propagates to
  // netstack at some future time. We have no way to observe that SHUT_RD has propagated (because
  // it propagates independently from SHUT_WR).
  ASSERT_EQ(shutdown(server().get(), SHUT_RDWR), 0) << strerror(errno);

  // Wait for the FIN to arrive at the client and for the state to propagate to the client's fdio.
  {
    pollfd pfd = {
        .fd = client().get(),
        .events = POLLRDHUP,
    };
    int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLRDHUP);
  }

  // Send data from the client(). The server should now very likely be in SHUT_RD and respond with
  // RST.
  char c;
  ASSERT_EQ(write(client().get(), &c, sizeof(c)), ssize_t(sizeof(c))) << strerror(errno);

  // Wait for the client to receive the RST and for the state to propagate through its fdio.
  pollfd pfd = {
      .fd = client().get(),
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLHUP | POLLERR);
}

// ShutdownPendingWrite tests for all of the application writes that
// occurred before shutdown SHUT_WR, to be received by the remote.
TEST_F(NetStreamSocketsTest, ShutdownPendingWrite) {
  // Fill the send buffer of the server socket so that we have some
  // pending data waiting to be sent out to the remote.
  ssize_t wrote;
  ASSERT_NO_FATAL_FAILURE(fill_stream_send_buf(server().get(), client().get(), &wrote));

  // SHUT_WR should enqueue a FIN after all of the application writes.
  EXPECT_EQ(shutdown(server().get(), SHUT_WR), 0) << strerror(errno);

  // All client reads are expected to return here, including the last
  // read on receiving a FIN. Keeping a timeout for unexpected failures.
  timeval tv = {
      .tv_sec = std::chrono::seconds(kTimeout).count(),
  };
  EXPECT_EQ(setsockopt(client().get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0)
      << strerror(errno);

  ssize_t rcvd = 0;
  // Keep a large enough buffer to reduce the number of read calls, as
  // we expect the receive buffer to be filled up at this point.
  char buf[4096];
  // Each read would make room for the server to send out more data
  // that has been enqueued from successful server socket writes.
  for (;;) {
    ssize_t ret = read(client().get(), &buf, sizeof(buf));
    ASSERT_GE(ret, 0) << strerror(errno);
    // Expect the last read to return 0 after the stack sees a FIN.
    if (ret == 0) {
      break;
    }
    rcvd += ret;
  }
  // Expect no data drops and all written data by server is received
  // by the client().
  EXPECT_EQ(rcvd, wrote);
}

using BlockedIOParams = std::tuple<IOMethod, CloseTarget, bool>;

class BlockedIOTest : public NetStreamSocketsTest,
                      public testing::WithParamInterface<BlockedIOParams> {};

TEST_P(BlockedIOTest, CloseWhileBlocked) {
  auto const& [io_method, close_target, linger_enabled] = GetParam();

  bool is_write = io_method.isWrite();

#if defined(__Fuchsia__)
  if (is_write) {
    GTEST_SKIP() << "TODO(https://fxbug.dev/60337): Enable socket write methods after we are able "
                    "to deterministically block on socket writes.";
  }
#endif

  // If linger is enabled, closing the socket will cause a TCP RST (by definition).
  bool close_rst = linger_enabled;
  if (is_write) {
    // Fill the send buffer of the client socket to cause write to block.
    ASSERT_NO_FATAL_FAILURE(fill_stream_send_buf(client().get(), server().get(), nullptr));
    // Buffes are full. Closing the socket will now cause a TCP RST.
    close_rst = true;
  }

  // While blocked in I/O, close the peer.
  std::latch fut_started(1);
  // NB: lambdas are not allowed to capture reference to local binding declared
  // in enclosing function.
  const auto fut = std::async(std::launch::async, [&, op = io_method]() {
    fut_started.count_down();

    char c;
    if (close_rst) {
      ASSERT_EQ(op.ExecuteIO(client().get(), &c, sizeof(c)), -1);
      EXPECT_EQ(errno, ECONNRESET) << strerror(errno);
    } else {
      ASSERT_EQ(op.ExecuteIO(client().get(), &c, sizeof(c)), 0) << strerror(errno);
    }
  });
  fut_started.wait();
  ASSERT_NO_FATAL_FAILURE(AssertBlocked(fut));

  // When enabled, causes `close` to send a TCP RST.
  linger opt = {
      .l_onoff = linger_enabled,
      .l_linger = 0,
  };

  switch (close_target) {
    case CloseTarget::CLIENT: {
      ASSERT_EQ(setsockopt(client().get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
          << strerror(errno);

      int fd = client().release();

      ASSERT_EQ(close(fd), 0) << strerror(errno);

      // Closing the file descriptor does not interrupt the pending I/O.
      ASSERT_NO_FATAL_FAILURE(AssertBlocked(fut));

      // The pending I/O is still blocked, but the file descriptor is gone.
      ASSERT_EQ(fsync(fd), -1) << strerror(errno);
      ASSERT_EQ(errno, EBADF) << errno;

      [[fallthrough]];  // to unblock the future.
    }
    case CloseTarget::SERVER: {
      ASSERT_EQ(setsockopt(server().get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
          << strerror(errno);
      ASSERT_EQ(close(server().release()), 0) << strerror(errno);
      break;
    }
  }
  ASSERT_EQ(fut.wait_for(kTimeout), std::future_status::ready);

#if !defined(__Fuchsia__)
  auto undo = DisableSigPipe(is_write);
#endif

  char c;
  switch (close_target) {
    case CloseTarget::CLIENT: {
      ASSERT_EQ(io_method.ExecuteIO(client().get(), &c, sizeof(c)), -1);
      EXPECT_EQ(errno, EBADF) << strerror(errno);
      break;
    }
    case CloseTarget::SERVER: {
      if (is_write) {
        ASSERT_EQ(io_method.ExecuteIO(client().get(), &c, sizeof(c)), -1);
        EXPECT_EQ(errno, EPIPE) << strerror(errno);
      } else {
        ASSERT_EQ(io_method.ExecuteIO(client().get(), &c, sizeof(c)), 0) << strerror(errno);
      }
      break;
    }
  }
}

std::string BlockedIOParamsToString(const testing::TestParamInfo<BlockedIOParams> info) {
  // NB: this is a freestanding function because ured binding declarations are not allowed in
  // lambdas.
  auto const& [io_method, close_target, linger_enabled] = info.param;
  std::stringstream s;
  s << "close" << CloseTargetToString(close_target) << "Linger";
  if (linger_enabled) {
    s << "Foreground";
  } else {
    s << "Background";
  }
  s << "During" << io_method.IOMethodToString();

  return s.str();
}

INSTANTIATE_TEST_SUITE_P(
    NetStreamTest, BlockedIOTest,
    testing::Combine(testing::Values(IOMethod::Op::READ, IOMethod::Op::READV, IOMethod::Op::RECV,
                                     IOMethod::Op::RECVFROM, IOMethod::Op::RECVMSG,
                                     IOMethod::Op::WRITE, IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                     IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                     testing::Values(CloseTarget::CLIENT, CloseTarget::SERVER),
                     testing::Values(false, true)),
    BlockedIOParamsToString);

// Use this routine to test blocking socket reads. On failure, this attempts to recover the
// blocked thread. Return value:
//      (1) actual length of read data on successful recv
//      (2) 0, when we abort a blocked recv
//      (3) -1, on failure of both of the above operations.
ssize_t asyncSocketRead(int recvfd, int sendfd, char* buf, ssize_t len, int flags,
                        sockaddr_in* addr, const socklen_t* addrlen, int socket_type,
                        std::chrono::duration<double> timeout) {
  std::future<ssize_t> recv = std::async(std::launch::async, [recvfd, buf, len, flags]() {
    memset(buf, 0xde, len);
    return recvfrom(recvfd, buf, len, flags, nullptr, nullptr);
  });

  if (recv.wait_for(timeout) == std::future_status::ready) {
    return recv.get();
  }

  // recover the blocked receiver thread
  switch (socket_type) {
    case SOCK_STREAM: {
      // shutdown() would unblock the receiver thread with recv returning 0.
      EXPECT_EQ(shutdown(recvfd, SHUT_RD), 0) << strerror(errno);
      // We do not use 'timeout' because that maybe short here. We expect to succeed and hence use
      // a known large timeout to ensure the test does not hang in case underlying code is broken.
      EXPECT_EQ(recv.wait_for(kTimeout), std::future_status::ready);
      EXPECT_EQ(recv.get(), 0);
      break;
    }
    case SOCK_DGRAM: {
      // Send a 0 length payload to unblock the receiver.
      // This would ensure that the async-task deterministically exits before call to future`s
      // destructor. Calling close(.release()) on recvfd when the async task is blocked on recv(),
      // __does_not__ cause recv to return; this can result in undefined behavior, as the
      // descriptor can get reused. Instead of sending a valid packet to unblock the recv() task,
      // we could call shutdown(), but that returns ENOTCONN (unconnected) but still causing
      // recv() to return. shutdown() becomes unreliable for unconnected UDP sockets because,
      // irrespective of the effect of calling this call, it returns error.
      EXPECT_EQ(sendto(sendfd, nullptr, 0, 0, reinterpret_cast<sockaddr*>(addr), *addrlen), 0)
          << strerror(errno);
      // We use a known large timeout for the same reason as for the above case.
      EXPECT_EQ(recv.wait_for(kTimeout), std::future_status::ready);
      EXPECT_EQ(recv.get(), 0);
      break;
    }
    default: {
      return -1;
    }
  }
  return 0;
}

class DatagramSendTest : public testing::TestWithParam<IOMethod> {};

TEST_P(DatagramSendTest, SendToIPv4MappedIPv6FromIPv4) {
  auto io_method = GetParam();

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  sockaddr_in6 addr6 = {
      .sin6_family = AF_INET6,
      .sin6_port = addr.sin_port,
  };
  addr6.sin6_addr.s6_addr[10] = 0xff;
  addr6.sin6_addr.s6_addr[11] = 0xff;
  memcpy(&addr6.sin6_addr.s6_addr[12], &addr.sin_addr.s_addr, sizeof(addr.sin_addr.s_addr));

  char buf[INET6_ADDRSTRLEN];
  ASSERT_TRUE(IN6_IS_ADDR_V4MAPPED(&addr6.sin6_addr))
      << inet_ntop(addr6.sin6_family, &addr6.sin6_addr, buf, sizeof(buf));

  switch (io_method.Op()) {
    case IOMethod::Op::SENDTO: {
      ASSERT_EQ(
          sendto(fd.get(), nullptr, 0, 0, reinterpret_cast<const sockaddr*>(&addr6), sizeof(addr6)),
          -1);
      ASSERT_EQ(errno, EAFNOSUPPORT) << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      msghdr msghdr = {
          .msg_name = &addr6,
          .msg_namelen = sizeof(addr6),
      };
      ASSERT_EQ(sendmsg(fd.get(), &msghdr, 0), -1);
      ASSERT_EQ(errno, EAFNOSUPPORT) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
}

TEST_P(DatagramSendTest, DatagramSend) {
  auto io_method = GetParam();
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  EXPECT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  EXPECT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  EXPECT_EQ(addrlen, sizeof(addr));

  std::string msg = "hello";
  char recvbuf[32] = {};
  iovec iov = {
      .iov_base = msg.data(),
      .iov_len = msg.size(),
  };
  msghdr msghdr = {
      .msg_name = &addr,
      .msg_namelen = addrlen,
      .msg_iov = &iov,
      .msg_iovlen = 1,
  };

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  switch (io_method.Op()) {
    case IOMethod::Op::SENDTO: {
      EXPECT_EQ(sendto(sendfd.get(), msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                       addrlen),
                ssize_t(msg.size()))
          << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      EXPECT_EQ(sendmsg(sendfd.get(), &msghdr, 0), ssize_t(msg.size())) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), 0, &addr,
                            &addrlen, SOCK_DGRAM, kTimeout),
            ssize_t(msg.size()));
  auto success_rcv_duration = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(std::string(recvbuf, msg.size()), msg);
  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);

  // sendto/sendmsg on connected sockets does accept sockaddr input argument and
  // also lets the dest sockaddr be overridden from what was passed for connect.
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  EXPECT_EQ(connect(sendfd.get(), reinterpret_cast<sockaddr*>(&addr), addrlen), 0)
      << strerror(errno);
  switch (io_method.Op()) {
    case IOMethod::Op::SENDTO: {
      EXPECT_EQ(sendto(sendfd.get(), msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                       addrlen),
                ssize_t(msg.size()))
          << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      EXPECT_EQ(sendmsg(sendfd.get(), &msghdr, 0), ssize_t(msg.size())) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), 0, &addr,
                            &addrlen, SOCK_DGRAM, kTimeout),
            ssize_t(msg.size()));
  EXPECT_EQ(std::string(recvbuf, msg.size()), msg);

  // Test sending to an address that is different from what we're connected to.
  //
  // We connect to a port that was emphemerally assigned which may fall anywhere
  // in [16000, UINT16_MAX] on gVisor's netstack-based platforms[1] or
  // [32768, 60999] on Linux platforms[2]. Adding 1 to UINT16_MAX will overflow
  // and result in a new port value of 0 so we always subtract by 1 as both
  // platforms that this test runs on will assign a port that will not
  // "underflow" when subtracting by 1 (as the port is always at least 1).
  // Previously, we added by 1 and this resulted in a test flake on Fuchsia
  // (gVisor netstack-based). See https://fxbug.dev/84431 for more details.
  //
  // [1]:
  // https://github.com/google/gvisor/blob/570ca571805d6939c4c24b6a88660eefaf558ae7/pkg/tcpip/ports/ports.go#L242
  //
  // [2]: default ip_local_port_range setting, as per
  //      https://www.kernel.org/doc/Documentation/networking/ip-sysctl.txt
  const uint16_t orig_sin_port = addr.sin_port;
  addr.sin_port = htons(ntohs(orig_sin_port) - 1);
  switch (io_method.Op()) {
    case IOMethod::Op::SENDTO: {
      EXPECT_EQ(sendto(sendfd.get(), msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                       addrlen),
                ssize_t(msg.size()))
          << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      EXPECT_EQ(sendmsg(sendfd.get(), &msghdr, 0), ssize_t(msg.size())) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
  // Expect blocked receiver and try to recover it by sending a packet to the
  // original connected sockaddr.
  addr.sin_port = orig_sin_port;
  // As we expect failure, to keep the recv wait time minimal, we base it on the time taken for a
  // successful recv.
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), 0, &addr,
                            &addrlen, SOCK_DGRAM, success_rcv_duration * 10),
            0);

  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetDatagramTest, DatagramSendTest,
                         testing::Values(IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                         [](const testing::TestParamInfo<IOMethod>& info) {
                           return info.param.IOMethodToString();
                         });

TEST(NetDatagramTest, DatagramConnectWrite) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  const char msg[] = "hello";

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(sendfd.get(), reinterpret_cast<sockaddr*>(&addr), addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(write(sendfd.get(), msg, sizeof(msg)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_EQ(close(sendfd.release()), 0) << strerror(errno);

  pollfd pfd = {
      .fd = recvfd.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(recvfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, DatagramPartialRecv) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  const char kTestMsg[] = "hello";
  const int kTestMsgSize = sizeof(kTestMsg);

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(kTestMsgSize, sendto(sendfd.get(), kTestMsg, kTestMsgSize, 0,
                                 reinterpret_cast<sockaddr*>(&addr), addrlen));

  char recv_buf[kTestMsgSize];

  // Read only first 2 bytes of the message. recv() is expected to discard the
  // rest.
  const int kPartialReadSize = 2;

  iovec iov = {
      .iov_base = recv_buf,
      .iov_len = kPartialReadSize,
  };
  // TODO(https://github.com/google/sanitizers/issues/1455): The size of this
  // array should be 0 or 1, but ASAN's recvmsg interceptor incorrectly encodes
  // that recvmsg writes [msg_name:][:msg_namelen'] (prime indicates value
  // after recvmsg returns), while the actual behavior is that
  // [msg_name:][:min(msg_namelen, msg_namelen'] is written.
  uint8_t from[sizeof(addr) + 1];
  msghdr msg = {
      .msg_name = from,
      .msg_namelen = sizeof(from),
      .msg_iov = &iov,
      .msg_iovlen = 1,
  };

  ASSERT_EQ(recvmsg(recvfd.get(), &msg, 0), kPartialReadSize);
  ASSERT_EQ(msg.msg_namelen, sizeof(addr));
  ASSERT_EQ(std::string(kTestMsg, kPartialReadSize), std::string(recv_buf, kPartialReadSize));
  EXPECT_EQ(MSG_TRUNC, msg.msg_flags);

  // Send the second packet.
  ASSERT_EQ(kTestMsgSize, sendto(sendfd.get(), kTestMsg, kTestMsgSize, 0,
                                 reinterpret_cast<sockaddr*>(&addr), addrlen));

  // Read the whole packet now.
  recv_buf[0] = 0;
  iov.iov_len = sizeof(recv_buf);
  ASSERT_EQ(recvmsg(recvfd.get(), &msg, 0), kTestMsgSize);
  ASSERT_EQ(std::string(kTestMsg, kTestMsgSize), std::string(recv_buf, kTestMsgSize));
  EXPECT_EQ(msg.msg_flags, 0);

  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, POLLOUT) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  pollfd pfd = {
      .fd = fd.get(),
      .events = POLLOUT,
  };
  int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);

  EXPECT_EQ(close(fd.release()), 0) << strerror(errno);
}

// DatagramSendtoRecvfrom tests if UDP send automatically binds an ephemeral
// port where the receiver can responds to.
TEST(NetDatagramTest, DatagramSendtoRecvfrom) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  const char msg[] = "hello";

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(sendto(sendfd.get(), msg, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&addr), addrlen),
            ssize_t(sizeof(msg)))
      << strerror(errno);

  char buf[sizeof(msg) + 1] = {};

  sockaddr_in peer;
  socklen_t peerlen = sizeof(peer);
  ASSERT_EQ(
      recvfrom(recvfd.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peerlen),
      ssize_t(sizeof(msg)))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  ASSERT_EQ(sendto(recvfd.get(), buf, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&peer), peerlen),
            ssize_t(sizeof(msg)))
      << strerror(errno);

  ASSERT_EQ(
      recvfrom(sendfd.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peerlen),
      ssize_t(sizeof(msg)))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  char addrbuf[INET_ADDRSTRLEN], peerbuf[INET_ADDRSTRLEN];
  const char* addrstr = inet_ntop(addr.sin_family, &addr.sin_addr, addrbuf, sizeof(addrbuf));
  ASSERT_NE(addrstr, nullptr);
  const char* peerstr = inet_ntop(peer.sin_family, &peer.sin_addr, peerbuf, sizeof(peerbuf));
  ASSERT_NE(peerstr, nullptr);
  ASSERT_STREQ(peerstr, addrstr);

  ASSERT_EQ(close(sendfd.release()), 0) << strerror(errno);

  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

// DatagramSendtoRecvfromV6 tests if UDP send automatically binds an ephemeral
// port where the receiver can responds to.
TEST(NetDatagramTest, DatagramSendtoRecvfromV6) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, 0))) << strerror(errno);

  sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  const char msg[] = "hello";

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(sendto(sendfd.get(), msg, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&addr), addrlen),
            ssize_t(sizeof(msg)))
      << strerror(errno);

  char buf[sizeof(msg) + 1] = {};

  sockaddr_in6 peer;
  socklen_t peerlen = sizeof(peer);
  ASSERT_EQ(
      recvfrom(recvfd.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peerlen),
      ssize_t(sizeof(msg)))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  ASSERT_EQ(sendto(recvfd.get(), buf, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&peer), peerlen),
            ssize_t(sizeof(msg)))
      << strerror(errno);

  ASSERT_EQ(
      recvfrom(sendfd.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peerlen),
      ssize_t(sizeof(msg)))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  char addrbuf[INET6_ADDRSTRLEN], peerbuf[INET6_ADDRSTRLEN];
  const char* addrstr = inet_ntop(addr.sin6_family, &addr.sin6_addr, addrbuf, sizeof(addrbuf));
  ASSERT_NE(addrstr, nullptr);
  const char* peerstr = inet_ntop(peer.sin6_family, &peer.sin6_addr, peerbuf, sizeof(peerbuf));
  ASSERT_NE(peerstr, nullptr);
  ASSERT_STREQ(peerstr, addrstr);

  ASSERT_EQ(close(sendfd.release()), 0) << strerror(errno);

  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, ConnectUnspecV4) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  sockaddr_in addr = {
      .sin_family = AF_UNSPEC,
  };

  EXPECT_EQ(connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr),
                    offsetof(sockaddr_in, sin_family) + sizeof(addr.sin_family)),
            0)
      << strerror(errno);
  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, ConnectUnspecV6) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  sockaddr_in6 addr = {
      .sin6_family = AF_UNSPEC,
  };

  EXPECT_EQ(connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr),
                    offsetof(sockaddr_in6, sin6_family) + sizeof(addr.sin6_family)),
            0)
      << strerror(errno);
  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

// Note: we choose 100 because the max number of fds per process is limited to
// 256.
const int32_t kListeningSockets = 100;

TEST(NetStreamTest, MultipleListeningSockets) {
  fbl::unique_fd listenfds[kListeningSockets];
  fbl::unique_fd connfd[kListeningSockets];

  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  socklen_t addrlen = sizeof(addr);

  for (auto& listenfd : listenfds) {
    ASSERT_TRUE(listenfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    ASSERT_EQ(bind(listenfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);

    ASSERT_EQ(listen(listenfd.get(), 0), 0) << strerror(errno);
  }

  for (int i = 0; i < kListeningSockets; i++) {
    ASSERT_EQ(getsockname(listenfds[i].get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
        << strerror(errno);
    ASSERT_EQ(addrlen, sizeof(addr));

    ASSERT_TRUE(connfd[i] = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    ASSERT_EQ(connect(connfd[i].get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  for (int i = 0; i < kListeningSockets; i++) {
    ASSERT_EQ(0, close(connfd[i].release()));
    ASSERT_EQ(0, close(listenfds[i].release()));
  }
}

// Socket tests across multiple socket-types, SOCK_DGRAM, SOCK_STREAM.
class NetSocketTest : public testing::TestWithParam<int> {};

// Test MSG_PEEK
// MSG_PEEK : Peek into the socket receive queue without moving the contents from it.
//
// TODO(https://fxbug.dev/90876): change this test to use recvmsg instead of recvfrom to exercise
// MSG_PEEK with scatter/gather.
TEST_P(NetSocketTest, SocketPeekTest) {
  int socket_type = GetParam();
  sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };
  socklen_t addrlen = sizeof(addr);
  fbl::unique_fd sendfd;
  fbl::unique_fd recvfd;
  ssize_t expectReadLen = 0;
  char sendbuf[8] = {};
  char recvbuf[2 * sizeof(sendbuf)] = {};
  ssize_t sendlen = sizeof(sendbuf);

  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, socket_type, 0))) << strerror(errno);
  // Setup the sender and receiver sockets.
  switch (socket_type) {
    case SOCK_STREAM: {
      fbl::unique_fd acptfd;
      ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, socket_type, 0))) << strerror(errno);
      EXPECT_EQ(bind(acptfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
          << strerror(errno);
      EXPECT_EQ(getsockname(acptfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
          << strerror(errno);
      EXPECT_EQ(addrlen, sizeof(addr));
      EXPECT_EQ(listen(acptfd.get(), 0), 0) << strerror(errno);
      EXPECT_EQ(connect(sendfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
          << strerror(errno);
      ASSERT_TRUE(recvfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr)))
          << strerror(errno);
      EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
      // Expect to read both the packets in a single recv() call.
      expectReadLen = sizeof(recvbuf);
      break;
    }
    case SOCK_DGRAM: {
      ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, socket_type, 0))) << strerror(errno);
      EXPECT_EQ(bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0)
          << strerror(errno);
      EXPECT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&addr), &addrlen), 0)
          << strerror(errno);
      EXPECT_EQ(addrlen, sizeof(addr));
      // Expect to read single packet per recv() call.
      expectReadLen = sizeof(sendbuf);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
    }
  }

  // This test sends 2 packets with known values and validates MSG_PEEK across the 2 packets.
  sendbuf[0] = 0x56;
  sendbuf[6] = 0x78;

  // send 2 separate packets and test peeking across
  EXPECT_EQ(sendto(sendfd.get(), sendbuf, sizeof(sendbuf), 0,
                   reinterpret_cast<const sockaddr*>(&addr), addrlen),
            sendlen)
      << strerror(errno);
  EXPECT_EQ(sendto(sendfd.get(), sendbuf, sizeof(sendbuf), 0,
                   reinterpret_cast<const sockaddr*>(&addr), addrlen),
            sendlen)
      << strerror(errno);

  auto start = std::chrono::steady_clock::now();
  // First peek on first byte.
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, 1, MSG_PEEK, &addr, &addrlen,
                            socket_type, kTimeout),
            1);
  auto success_rcv_duration = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(recvbuf[0], sendbuf[0]);

  // Second peek across first 2 packets and drain them from the socket receive queue.
  ssize_t torecv = sizeof(recvbuf);
  for (int i = 0; torecv > 0; i++) {
    int flags = i % 2 ? 0 : MSG_PEEK;
    ssize_t readLen = 0;
    // Retry socket read with MSG_PEEK to ensure all of the expected data is received.
    //
    // TODO(https://fxbug.dev/74639) : Use SO_RCVLOWAT instead of retry.
    do {
      readLen = asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), flags, &addr,
                                &addrlen, socket_type, kTimeout);
      if (HasFailure()) {
        break;
      }
    } while (flags == MSG_PEEK && readLen < expectReadLen);
    EXPECT_EQ(readLen, expectReadLen);

    EXPECT_EQ(recvbuf[0], sendbuf[0]);
    EXPECT_EQ(recvbuf[6], sendbuf[6]);
    // For SOCK_STREAM, we validate peek across 2 packets with a single recv call.
    if (readLen == sizeof(recvbuf)) {
      EXPECT_EQ(recvbuf[8], sendbuf[0]);
      EXPECT_EQ(recvbuf[14], sendbuf[6]);
    }
    if (flags != MSG_PEEK) {
      torecv -= readLen;
    }
  }

  // Third peek on empty socket receive buffer, expect failure.
  //
  // As we expect failure, to keep the recv wait time minimal, we base it on the time taken for a
  // successful recv.
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, 1, MSG_PEEK, &addr, &addrlen,
                            socket_type, success_rcv_duration * 10),
            0);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetSocket, NetSocketTest, testing::Values(SOCK_DGRAM, SOCK_STREAM));

TEST_P(SocketKindTest, IoctlInterfaceLookupRoundTrip) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = NewSocket()) << strerror(errno);

  // This test assumes index 1 is bound to a valid interface. In Fuchsia's test environment (the
  // component executing this test), 1 is always bound to "lo".
  ifreq ifr_iton = {};
  ifr_iton.ifr_ifindex = 1;
  // Set ifr_name to random chars to test ioctl correctly sets null terminator.
  memset(ifr_iton.ifr_name, 0xde, IFNAMSIZ);
  ASSERT_EQ(strnlen(ifr_iton.ifr_name, IFNAMSIZ), (size_t)IFNAMSIZ);
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFNAME, &ifr_iton), 0) << strerror(errno);
  ASSERT_LT(strnlen(ifr_iton.ifr_name, IFNAMSIZ), (size_t)IFNAMSIZ);

  ifreq ifr_ntoi;
  strncpy(ifr_ntoi.ifr_name, ifr_iton.ifr_name, IFNAMSIZ);
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFINDEX, &ifr_ntoi), 0) << strerror(errno);
  EXPECT_EQ(ifr_ntoi.ifr_ifindex, 1);

  ifreq ifr_err;
  memset(ifr_err.ifr_name, 0xde, IFNAMSIZ);
  // Although the first few bytes of ifr_name contain the correct name, there is no null
  // terminator and the remaining bytes are gibberish, should match no interfaces.
  memcpy(ifr_err.ifr_name, ifr_iton.ifr_name, strnlen(ifr_iton.ifr_name, IFNAMSIZ));

  const struct {
    std::string name;
    int request;
  } requests[] = {
      {
          .name = "SIOCGIFINDEX",
          .request = SIOCGIFINDEX,
      },
      {
          .name = "SIOCGIFFLAGS",
          .request = SIOCGIFFLAGS,
      },
  };
  for (const auto& request : requests) {
    ASSERT_EQ(ioctl(fd.get(), request.request, &ifr_err), -1) << request.name;
    EXPECT_EQ(errno, ENODEV) << request.name << ": " << strerror(errno);
  }
}

TEST_P(SocketKindTest, IoctlInterfaceNotFound) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = NewSocket()) << strerror(errno);

  // Invalid ifindex "-1" should match no interfaces.
  ifreq ifr_iton = {};
  ifr_iton.ifr_ifindex = -1;
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFNAME, &ifr_iton), -1);
  EXPECT_EQ(errno, ENODEV) << strerror(errno);

  // Empty name should match no interface.
  ifreq ifr = {};
  const struct {
    std::string name;
    int request;
  } requests[] = {
      {
          .name = "SIOCGIFINDEX",
          .request = SIOCGIFINDEX,
      },
      {
          .name = "SIOCGIFFLAGS",
          .request = SIOCGIFFLAGS,
      },
  };
  for (const auto& request : requests) {
    ASSERT_EQ(ioctl(fd.get(), request.request, &ifr), -1) << request.name;
    EXPECT_EQ(errno, ENODEV) << request.name << ": " << strerror(errno);
  }
}

template <typename F>
void TestGetname(const fbl::unique_fd& fd, F getname, const sockaddr* sa, const socklen_t sa_len) {
  ASSERT_EQ(getname(fd.get(), nullptr, nullptr), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  errno = 0;

  sockaddr_storage ss;
  ASSERT_EQ(getname(fd.get(), reinterpret_cast<sockaddr*>(&ss), nullptr), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  errno = 0;

  socklen_t len = 0;
  ASSERT_EQ(getname(fd.get(), nullptr, &len), 0) << strerror(errno);
  EXPECT_EQ(len, sa_len);

  len = 1;
  ASSERT_EQ(getname(fd.get(), nullptr, &len), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  EXPECT_EQ(len, 1u);
  errno = 0;

  sa_family_t family;
  len = sizeof(family);
  ASSERT_EQ(getname(fd.get(), reinterpret_cast<sockaddr*>(&family), &len), 0) << strerror(errno);
  ASSERT_EQ(len, sa_len);
  EXPECT_EQ(family, sa->sa_family);

  len = sa_len;
  ASSERT_EQ(getname(fd.get(), reinterpret_cast<sockaddr*>(&ss), &len), 0) << strerror(errno);
  ASSERT_EQ(len, sa_len);
  EXPECT_EQ(memcmp(&ss, sa, sa_len), 0);

  struct {
    sockaddr_storage ss;
    char unused;
  } ss_with_extra = {
      .unused = 0x44,
  };
  len = sizeof(ss_with_extra);
  ASSERT_EQ(getname(fd.get(), reinterpret_cast<sockaddr*>(&ss_with_extra), &len), 0)
      << strerror(errno);
  ASSERT_EQ(len, sa_len);
  EXPECT_EQ(memcmp(&ss, sa, sa_len), 0);
  EXPECT_EQ(ss_with_extra.unused, 0x44);
}

TEST_P(SocketKindTest, Getsockname) {
  socklen_t len;
  sockaddr_storage ss;
  LoopbackAddr(&ss, &len);

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = NewSocket()) << strerror(errno);

  ASSERT_EQ(bind(fd.get(), reinterpret_cast<sockaddr*>(&ss), sizeof(ss)), 0) << strerror(errno);
  socklen_t ss_len = sizeof(ss);
  // Get the socket's local address so TestGetname can compare against it.
  ASSERT_EQ(getsockname(fd.get(), reinterpret_cast<sockaddr*>(&ss), &ss_len), 0) << strerror(errno);
  ASSERT_EQ(ss_len, len);

  ASSERT_NO_FATAL_FAILURE(TestGetname(fd, getsockname, reinterpret_cast<sockaddr*>(&ss), len));
}

TEST_P(SocketKindTest, Getpeername) {
  auto const& [domain, protocol] = GetParam();

  socklen_t len;
  sockaddr_storage ss;
  LoopbackAddr(&ss, &len);

  fbl::unique_fd listener;
  ASSERT_TRUE(listener = NewSocket()) << strerror(errno);
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<sockaddr*>(&ss), sizeof(ss)), 0)
      << strerror(errno);
  socklen_t ss_len = sizeof(ss);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&ss), &ss_len), 0)
      << strerror(errno);
  if (protocol == SOCK_STREAM) {
    ASSERT_EQ(listen(listener.get(), 1), 0) << strerror(errno);
  }

  fbl::unique_fd client;
  ASSERT_TRUE(client = NewSocket()) << strerror(errno);
  ASSERT_EQ(connect(client.get(), reinterpret_cast<sockaddr*>(&ss), sizeof(ss)), 0)
      << strerror(errno);

  ASSERT_NO_FATAL_FAILURE(TestGetname(client, getpeername, reinterpret_cast<sockaddr*>(&ss), len));
}

TEST(SocketKindTest, IoctlLookupForNonSocketFd) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(open("/", O_RDONLY | O_DIRECTORY))) << strerror(errno);

  ifreq ifr_iton = {};
  ifr_iton.ifr_ifindex = 1;
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFNAME, &ifr_iton), -1);
  EXPECT_EQ(errno, ENOTTY) << strerror(errno);

  ifreq ifr;
  strcpy(ifr.ifr_name, "loblah");
  const struct {
    std::string name;
    int request;
  } requests[] = {
      {
          .name = "SIOCGIFINDEX",
          .request = SIOCGIFINDEX,
      },
      {
          .name = "SIOCGIFFLAGS",
          .request = SIOCGIFFLAGS,
      },
  };
  for (const auto& request : requests) {
    ASSERT_EQ(ioctl(fd.get(), request.request, &ifr), -1) << request.name;
    EXPECT_EQ(errno, ENOTTY) << request.name << ": " << strerror(errno);
  }
}

TEST(IoctlTest, IoctlGetInterfaceFlags) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  ifreq ifr_ntof = {};
  {
    constexpr char name[] = "lo";
    memcpy(ifr_ntof.ifr_name, name, sizeof(name));
  }
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFFLAGS, &ifr_ntof), 0) << strerror(errno);
  const struct {
    std::string name;
    uint16_t bitmask;
    bool value;
  } flags[] = {
      {
          .name = "IFF_UP",
          .bitmask = IFF_UP,
          .value = true,
      },
      {
          .name = "IFF_LOOPBACK",
          .bitmask = IFF_LOOPBACK,
          .value = true,
      },
      {
          .name = "IFF_RUNNING",
          .bitmask = IFF_RUNNING,
          .value = true,
      },
      {
          .name = "IFF_PROMISC",
          .bitmask = IFF_PROMISC,
          .value = false,
      },
  };
  for (const auto& flag : flags) {
    EXPECT_EQ(static_cast<bool>(ifr_ntof.ifr_flags & flag.bitmask), flag.value)
        << std::bitset<16>(ifr_ntof.ifr_flags) << ", " << std::bitset<16>(flag.bitmask);
  }
  // Don't check strict equality of `ifr_ntof.ifr_flags` with expected flag
  // values, except on Fuchsia, because gVisor does not set all the interface
  // flags that Linux does.
#if defined(__Fuchsia__)
  uint16_t expected_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_MULTICAST;
  ASSERT_EQ(ifr_ntof.ifr_flags, expected_flags)
      << std::bitset<16>(ifr_ntof.ifr_flags) << ", " << std::bitset<16>(expected_flags);
#endif
}

TEST(IoctlTest, IoctlGetInterfaceAddressesNullIfConf) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  ASSERT_EQ(ioctl(fd.get(), SIOCGIFCONF, nullptr), -1);
  ASSERT_EQ(errno, EFAULT) << strerror(errno);
}

TEST(IoctlTest, IoctlGetInterfaceAddressesPartialRecord) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  // Get the interface configuration information, but only pass an `ifc_len`
  // large enough to hold a partial `struct ifreq`, and ensure that the buffer
  // is not overwritten.
  constexpr char kGarbage = 0xa;
  ifreq ifr;
  memset(&ifr, kGarbage, sizeof(ifr));
  ifconf ifc = {};
  ifc.ifc_len = sizeof(ifr) - 1;
  ifc.ifc_req = &ifr;

  ASSERT_EQ(ioctl(fd.get(), SIOCGIFCONF, &ifc), 0) << strerror(errno);
  ASSERT_EQ(ifc.ifc_len, 0);
  char* buffer = reinterpret_cast<char*>(&ifr);
  for (size_t i = 0; i < sizeof(ifr); i++) {
    EXPECT_EQ(buffer[i], kGarbage) << i;
  }
}

INSTANTIATE_TEST_SUITE_P(NetSocket, SocketKindTest,
                         testing::Combine(testing::Values(AF_INET, AF_INET6),
                                          testing::Values(SOCK_DGRAM, SOCK_STREAM)),
                         SocketKindToString);

using DomainProtocol = std::tuple<int, int>;
class IcmpSocketTest : public testing::TestWithParam<DomainProtocol> {
 protected:
  void SetUp() override {
#if !defined(__Fuchsia__)
    if (!IsRoot()) {
      GTEST_SKIP() << "This test requires root";
    }
#endif
    auto const& [domain, protocol] = GetParam();
    ASSERT_TRUE(fd_ = fbl::unique_fd(socket(domain, SOCK_DGRAM, protocol))) << strerror(errno);
  }

  const fbl::unique_fd& fd() const { return fd_; }

 private:
  fbl::unique_fd fd_;
};

TEST_P(IcmpSocketTest, GetSockoptSoProtocol) {
  auto const& [domain, protocol] = GetParam();

  int opt;
  socklen_t optlen = sizeof(opt);
  ASSERT_EQ(getsockopt(fd().get(), SOL_SOCKET, SO_PROTOCOL, &opt, &optlen), 0) << strerror(errno);
  EXPECT_EQ(optlen, sizeof(opt));
  EXPECT_EQ(opt, protocol);
}

TEST_P(IcmpSocketTest, PayloadIdentIgnored) {
  auto const& [domain, protocol] = GetParam();

  constexpr short kBindIdent = 3;
  constexpr short kDestinationIdent = kBindIdent + 1;

  switch (domain) {
    case AF_INET: {
      const sockaddr_in bind_addr = {
          .sin_family = AF_INET,
          .sin_port = htons(kBindIdent),
          .sin_addr =
              {
                  .s_addr = htonl(INADDR_LOOPBACK),
              },
      };
      ASSERT_EQ(bind(fd().get(), reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)),
                0)
          << strerror(errno);
      const icmphdr pkt = []() {
        icmphdr pkt;
        // Populate with garbage to prove other fields are unused.
        memset(&pkt, 0x4a, sizeof(pkt));
        pkt.type = ICMP_ECHO;
        pkt.code = 0;
        return pkt;
      }();
      const sockaddr_in dst_addr = {
          .sin_family = bind_addr.sin_family,
          .sin_port = htons(kDestinationIdent),
          .sin_addr = bind_addr.sin_addr,
      };
      ASSERT_EQ(sendto(fd().get(), &pkt, sizeof(pkt), 0,
                       reinterpret_cast<const sockaddr*>(&dst_addr), sizeof(dst_addr)),
                ssize_t(sizeof(pkt)))
          << strerror(errno);

      struct {
        std::remove_const<decltype(pkt)>::type hdr;
        char unused;
      } hdr_with_extra = {
          .unused = 0x44,
      };
      memset(&hdr_with_extra.hdr, 0x4a, sizeof(hdr_with_extra.hdr));
      ASSERT_EQ(read(fd().get(), &hdr_with_extra, sizeof(hdr_with_extra)), ssize_t(sizeof(pkt)))
          << strerror(errno);
      EXPECT_EQ(hdr_with_extra.unused, 0x44);
      EXPECT_EQ(hdr_with_extra.hdr.type, 0);
      EXPECT_EQ(hdr_with_extra.hdr.code, 0);
      EXPECT_NE(hdr_with_extra.hdr.checksum, 0);
      EXPECT_EQ(htons(hdr_with_extra.hdr.un.echo.id), kBindIdent);
      EXPECT_EQ(hdr_with_extra.hdr.un.echo.sequence, pkt.un.echo.sequence);
    } break;
    case AF_INET6: {
      const sockaddr_in6 bind_addr = {
          .sin6_family = AF_INET6,
          .sin6_port = htons(kBindIdent),
          .sin6_addr = IN6ADDR_LOOPBACK_INIT,
      };
      ASSERT_EQ(bind(fd().get(), reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)),
                0)
          << strerror(errno);
      const icmp6_hdr pkt = []() {
        icmp6_hdr pkt;
        // Populate with garbage to prove other fields are unused.
        memset(&pkt, 0x4a, sizeof(pkt));
        pkt.icmp6_type = ICMP6_ECHO_REQUEST;
        pkt.icmp6_code = 0;
        return pkt;
      }();
      const sockaddr_in6 dst_addr = {
          .sin6_family = bind_addr.sin6_family,
          .sin6_port = htons(kDestinationIdent),
          .sin6_addr = bind_addr.sin6_addr,
      };
      ASSERT_EQ(sendto(fd().get(), &pkt, sizeof(pkt), 0,
                       reinterpret_cast<const sockaddr*>(&dst_addr), sizeof(dst_addr)),
                ssize_t(sizeof(pkt)))
          << strerror(errno);

      struct {
        std::remove_const<decltype(pkt)>::type hdr;
        char unused;
      } hdr_with_extra = {
          .unused = 0x44,
      };
      memset(&hdr_with_extra.hdr, 0x4a, sizeof(hdr_with_extra.hdr));
      ASSERT_EQ(read(fd().get(), &hdr_with_extra, sizeof(hdr_with_extra)), ssize_t(sizeof(pkt)))
          << strerror(errno);
      EXPECT_EQ(hdr_with_extra.unused, 0x44);
      EXPECT_EQ(hdr_with_extra.hdr.icmp6_type, ICMP6_ECHO_REPLY);
      EXPECT_EQ(hdr_with_extra.hdr.icmp6_code, 0);
      EXPECT_NE(hdr_with_extra.hdr.icmp6_cksum, 0);
      EXPECT_EQ(htons(hdr_with_extra.hdr.icmp6_id), kBindIdent);
      EXPECT_EQ(hdr_with_extra.hdr.icmp6_seq, pkt.icmp6_seq);
    } break;
    default:
      FAIL() << "unknown domain";
  }
}

INSTANTIATE_TEST_SUITE_P(NetSocket, IcmpSocketTest,
                         testing::Values(std::make_pair(AF_INET, IPPROTO_ICMP),
                                         std::make_pair(AF_INET6, IPPROTO_ICMPV6)));

TEST(NetDatagramTest, PingIpv4LoopbackAddresses) {
  const char msg[] = "hello";
  char addrbuf[INET_ADDRSTRLEN];
  std::array<int, 5> sampleAddrOctets = {0, 1, 100, 200, 255};
  for (auto i : sampleAddrOctets) {
    for (auto j : sampleAddrOctets) {
      for (auto k : sampleAddrOctets) {
        // Skip the subnet and broadcast addresses.
        if ((i == 0 && j == 0 && k == 0) || (i == 255 && j == 255 && k == 255)) {
          continue;
        }
        // loopback_addr = 127.i.j.k
        in_addr loopback_sin_addr = {
            .s_addr = htonl((127 << 24) + (i << 16) + (j << 8) + k),
        };
        const char* loopback_addrstr =
            inet_ntop(AF_INET, &loopback_sin_addr, addrbuf, sizeof(addrbuf));
        ASSERT_NE(nullptr, loopback_addrstr);

        fbl::unique_fd recvfd;
        ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
        sockaddr_in rcv_addr = {
            .sin_family = AF_INET,
            .sin_addr = loopback_sin_addr,
        };
        ASSERT_EQ(
            bind(recvfd.get(), reinterpret_cast<const sockaddr*>(&rcv_addr), sizeof(rcv_addr)), 0)
            << "recvaddr=" << loopback_addrstr << ": " << strerror(errno);

        socklen_t rcv_addrlen = sizeof(rcv_addr);
        ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<sockaddr*>(&rcv_addr), &rcv_addrlen),
                  0)
            << strerror(errno);
        ASSERT_EQ(sizeof(rcv_addr), rcv_addrlen);

        fbl::unique_fd sendfd;
        ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
        sockaddr_in sendto_addr = {
            .sin_family = AF_INET,
            .sin_port = rcv_addr.sin_port,
            .sin_addr = loopback_sin_addr,
        };
        ASSERT_EQ(sendto(sendfd.get(), msg, sizeof(msg), 0,
                         reinterpret_cast<sockaddr*>(&sendto_addr), sizeof(sendto_addr)),
                  ssize_t(sizeof(msg)))
            << "sendtoaddr=" << loopback_addrstr << ": " << strerror(errno);
        EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);

        pollfd pfd = {
            .fd = recvfd.get(),
            .events = POLLIN,
        };
        int n = poll(&pfd, 1, std::chrono::milliseconds(kTimeout).count());
        ASSERT_GE(n, 0) << strerror(errno);
        ASSERT_EQ(n, 1);
        char buf[sizeof(msg) + 1] = {};
        ASSERT_EQ(read(recvfd.get(), buf, sizeof(buf)), ssize_t(sizeof(msg))) << strerror(errno);
        ASSERT_STREQ(buf, msg);

        EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
      }
    }
  }
}

struct CmsgSocketOption {
  int level;
  int cmsg_type;
  int optname_to_enable_receive;
};

using SocketDomainAndOption = std::tuple<sa_family_t, CmsgSocketOption>;

std::string SocketDomainAndOptionToString(
    const testing::TestParamInfo<SocketDomainAndOption>& info) {
  auto const& [domain, cmsg_sockopt] = info.param;
  auto const& [level, cmsg_type, optname_to_enable_receive] = cmsg_sockopt;

  std::string option_str = [](const int level, const int type) -> std::string {
    switch (level) {
      case SOL_SOCKET:
        return "SOL_SOCKET_" + [type]() -> std::string {
          switch (type) {
            case SO_TIMESTAMP:
              return "SO_TIMESTAMP";
            case SO_TIMESTAMPNS:
              return "SO_TIMESTAMPNS";
            default:
              return std::to_string(type);
          }
        }();
      case SOL_IP:
        return "SOL_IP_" + [type]() -> std::string {
          switch (type) {
            case IP_RECVTOS:
              return "IP_RECVTOS";
            default:
              return std::to_string(type);
          }
        }();
      default:
        return std::to_string(level) + "_" + std::to_string(type);
    }
  }(level, cmsg_type);

  return socketDomainToString(domain) + "_" + option_str;
}

class NetDatagramSocketsCmsgTestBase {
 protected:
  void SetUpDatagramSockets(sa_family_t domain) {
    ASSERT_TRUE(bound_ = fbl::unique_fd(socket(domain, SOCK_DGRAM, 0))) << strerror(errno);

    auto addr_info = [domain]() -> std::optional<std::pair<sockaddr_storage, unsigned int>> {
      sockaddr_storage addr{
          .ss_family = domain,
      };
      switch (domain) {
        case AF_INET: {
          auto sin = reinterpret_cast<sockaddr_in*>(&addr);
          sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
          sin->sin_port = 0;  // Automatically pick a port.
          return std::make_pair(addr, sizeof(sockaddr_in));
        }
        case AF_INET6: {
          auto sin6 = reinterpret_cast<sockaddr_in6*>(&addr);
          sin6->sin6_addr = IN6ADDR_LOOPBACK_INIT;
          sin6->sin6_port = 0;  // Automatically pick a port.
          return std::make_pair(addr, sizeof(sockaddr_in6));
        }
        default: {
          return std::nullopt;
        }
      }
    }();
    if (!addr_info.has_value()) {
      FAIL() << "unexpected test variant";
    }
    auto [addr, addrlen] = addr_info.value();
    ASSERT_EQ(bind(bound_.get(), reinterpret_cast<const sockaddr*>(&addr), addrlen), 0)
        << strerror(errno);

    {
      socklen_t bound_addrlen = addrlen;
      ASSERT_EQ(getsockname(bound_.get(), reinterpret_cast<sockaddr*>(&addr), &bound_addrlen), 0)
          << strerror(errno);
      ASSERT_EQ(addrlen, bound_addrlen);
    }

    ASSERT_TRUE(connected_ = fbl::unique_fd(socket(domain, SOCK_DGRAM, 0))) << strerror(errno);
    ASSERT_EQ(connect(connected_.get(), reinterpret_cast<sockaddr*>(&addr), addrlen), 0)
        << strerror(errno);
  }

  void TearDownDatagramSockets() {
    EXPECT_EQ(close(connected_.release()), 0) << strerror(errno);
    EXPECT_EQ(close(bound_.release()), 0) << strerror(errno);
  }

  template <typename F>
  void ReceiveAndCheckMessage(const char* sent_buf, ssize_t sent_buf_len, void* control,
                              socklen_t control_len, F check) const {
    char recv_buf[sent_buf_len + 1];
    iovec iovec = {
        .iov_base = recv_buf,
        .iov_len = sizeof(recv_buf),
    };
    msghdr msghdr = {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = control_len,
    };
    ASSERT_EQ(recvmsg(bound().get(), &msghdr, 0), ssize_t(sent_buf_len)) << strerror(errno);
    ASSERT_EQ(memcmp(recv_buf, sent_buf, sent_buf_len), 0);
    check(msghdr);
  }

  template <typename F>
  void SendAndCheckReceivedMessage(void* control, socklen_t control_len, F check) {
    constexpr char send_buf[] = "hello";

    ASSERT_EQ(send(connected().get(), send_buf, sizeof(send_buf), 0), ssize_t(sizeof(send_buf)))
        << strerror(errno);

    ReceiveAndCheckMessage(send_buf, sizeof(send_buf), control, control_len, check);
  }

  fbl::unique_fd const& bound() const { return bound_; }

  fbl::unique_fd const& connected() const { return connected_; }

 private:
  fbl::unique_fd bound_;
  fbl::unique_fd connected_;
};

class NetDatagramSocketsCmsgRecvTest : public NetDatagramSocketsCmsgTestBase,
                                       public testing::TestWithParam<SocketDomainAndOption> {
 protected:
  void SetUp() override {
    auto const& [domain, cmsg_sockopt] = GetParam();
    auto const& [level, cmsg_type, optname_to_enable_receive] = cmsg_sockopt;

    ASSERT_NO_FATAL_FAILURE(SetUpDatagramSockets(domain));

    // Enable the specified socket option.
    const int optval = 1;
    ASSERT_EQ(setsockopt(bound().get(), level, optname_to_enable_receive, &optval, sizeof(optval)),
              0)
        << strerror(errno);
  }

  void TearDown() override {
    if (!IsSkipped()) {
      EXPECT_NO_FATAL_FAILURE(TearDownDatagramSockets());
    }
  }
};

TEST_P(NetDatagramSocketsCmsgRecvTest, NullPtrNoControlMessages) {
  ASSERT_NO_FATAL_FAILURE(SendAndCheckReceivedMessage(nullptr, 1337, [](msghdr& msghdr) {
    // The msg_controllen field should be reset when the control buffer is null, even when no
    // control messages are enabled.
    EXPECT_EQ(msghdr.msg_controllen, 0u);
    EXPECT_EQ(CMSG_FIRSTHDR(&msghdr), nullptr);
  }));
}

TEST_P(NetDatagramSocketsCmsgRecvTest, NullControlBuffer) {
  ASSERT_NO_FATAL_FAILURE(SendAndCheckReceivedMessage(nullptr, 1337, [](msghdr& msghdr) {
    // The msg_controllen field should be reset when the control buffer is null.
    EXPECT_EQ(msghdr.msg_controllen, 0u);
    EXPECT_EQ(CMSG_FIRSTHDR(&msghdr), nullptr);
  }));
}

TEST_P(NetDatagramSocketsCmsgRecvTest, OneByteControlLength) {
  char control[1];
  ASSERT_NO_FATAL_FAILURE(SendAndCheckReceivedMessage(control, sizeof(control), [](msghdr& msghdr) {
    // If there is not enough space to store the cmsghdr, then nothing is stored.
    EXPECT_EQ(msghdr.msg_controllen, 0u);
    EXPECT_EQ(CMSG_FIRSTHDR(&msghdr), nullptr);
  }));
}

TEST_P(NetDatagramSocketsCmsgRecvTest, ZeroControlLength) {
  char control[1];
  ASSERT_NO_FATAL_FAILURE(SendAndCheckReceivedMessage(control, 0, [](msghdr& msghdr) {
    // The msg_controllen field should remain zero when no messages were written.
    EXPECT_EQ(msghdr.msg_controllen, 0u);
    EXPECT_EQ(CMSG_FIRSTHDR(&msghdr), nullptr);
  }));
}

TEST_P(NetDatagramSocketsCmsgRecvTest, FailureDoesNotResetControlLength) {
  char recvbuf[1];
  iovec iovec = {
      .iov_base = recvbuf,
      .iov_len = sizeof(recvbuf),
  };
  char control[1337];
  msghdr msghdr = {
      .msg_name = nullptr,
      .msg_namelen = 0,
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
  };
  ASSERT_EQ(recvmsg(bound().get(), &msghdr, MSG_DONTWAIT), -1);
  EXPECT_EQ(errno, EWOULDBLOCK) << strerror(errno);
  // The msg_controllen field should be left unchanged when recvmsg() fails for any reason.
  EXPECT_EQ(msghdr.msg_controllen, sizeof(control));
}

TEST_P(NetDatagramSocketsCmsgRecvTest, TruncatedMessage) {
  // A control message can be truncated if there is at least enough space to store the cmsghdr.
  char control[sizeof(cmsghdr)];
  ASSERT_NO_FATAL_FAILURE(SendAndCheckReceivedMessage(control, sizeof(cmsghdr), [](msghdr& msghdr) {
#if defined(__Fuchsia__)
    // TODO(https://fxbug.dev/86146): Add support for truncated control messages (MSG_CTRUNC).
    EXPECT_EQ(msghdr.msg_controllen, 0u);
    EXPECT_EQ(CMSG_FIRSTHDR(&msghdr), nullptr);
#else
    ASSERT_EQ(msghdr.msg_controllen, sizeof(cmsghdr));
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
    ASSERT_NE(cmsg, nullptr);
    EXPECT_EQ(cmsg->cmsg_len, sizeof(cmsghdr));
    auto const& cmsg_socketopt = std::get<1>(GetParam());
    EXPECT_EQ(cmsg->cmsg_level, cmsg_socketopt.level);
    EXPECT_EQ(cmsg->cmsg_type, cmsg_socketopt.cmsg_type);
#endif
  }));
}

INSTANTIATE_TEST_SUITE_P(NetDatagramSocketsCmsgRecvTests, NetDatagramSocketsCmsgRecvTest,
                         testing::Combine(testing::Values(AF_INET, AF_INET6),
                                          testing::Values(
                                              CmsgSocketOption{
                                                  .level = SOL_SOCKET,
                                                  .cmsg_type = SO_TIMESTAMP,
                                                  .optname_to_enable_receive = SO_TIMESTAMP,
                                              },
                                              CmsgSocketOption{
                                                  .level = SOL_SOCKET,
                                                  .cmsg_type = SO_TIMESTAMPNS,
                                                  .optname_to_enable_receive = SO_TIMESTAMPNS,
                                              })),
                         SocketDomainAndOptionToString);

INSTANTIATE_TEST_SUITE_P(NetDatagramSocketsCmsgRecvIPv4Tests, NetDatagramSocketsCmsgRecvTest,
                         testing::Combine(testing::Values(AF_INET),
                                          testing::Values(CmsgSocketOption{
                                              .level = SOL_IP,
                                              .cmsg_type = IP_TOS,
                                              .optname_to_enable_receive = IP_RECVTOS,
                                          })),
                         SocketDomainAndOptionToString);

class NetDatagramSocketsCmsgSendTest : public NetDatagramSocketsCmsgTestBase,
                                       public testing::TestWithParam<sa_family_t> {
 protected:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpDatagramSockets(GetParam())); }

  cmsghdr OrdinaryControlMessage() {
    return {
        // SOL_SOCKET/SCM_RIGHTS is used for general cmsg tests, because SOL_SOCKET messages are
        // supported on every socket type, and the SCM_RIGHTS control message is a no-op.
        // https://github.com/torvalds/linux/blob/42eb8fdac2f/net/core/sock.c#L2628
        .cmsg_len = CMSG_LEN(0),
        .cmsg_level = SOL_SOCKET,
        .cmsg_type = SCM_RIGHTS,
    };
  }
};

TEST_P(NetDatagramSocketsCmsgSendTest, NullControlBufferWithNonZeroLength) {
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = nullptr,
      .msg_controllen = 1,
  };

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), -1);
  ASSERT_EQ(errno, EFAULT) << strerror(errno);
}

TEST_P(NetDatagramSocketsCmsgSendTest, NonNullControlBufferWithZeroLength) {
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  uint8_t send_control[1];
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = send_control,
      .msg_controllen = 0,
  };

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), ssize_t(sizeof(send_buf)))
      << strerror(errno);

  ASSERT_NO_FATAL_FAILURE(
      ReceiveAndCheckMessage(send_buf, sizeof(send_buf), nullptr, 0, [](msghdr& recv_msghdr) {
        EXPECT_EQ(recv_msghdr.msg_controllen, 0u);
        ASSERT_EQ(CMSG_FIRSTHDR(&recv_msghdr), nullptr);
      }));
}

TEST_P(NetDatagramSocketsCmsgSendTest, ValidCmsg) {
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  cmsghdr cmsg = OrdinaryControlMessage();
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = &cmsg,
      .msg_controllen = cmsg.cmsg_len,
  };

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), ssize_t(sizeof(send_buf)))
      << strerror(errno);
  uint8_t recv_control[CMSG_SPACE(0)];
  ASSERT_NO_FATAL_FAILURE(ReceiveAndCheckMessage(send_buf, sizeof(send_buf), recv_control,
                                                 sizeof(recv_control), [](msghdr& recv_msghdr) {
                                                   EXPECT_EQ(recv_msghdr.msg_controllen, 0u);
                                                   ASSERT_EQ(CMSG_FIRSTHDR(&recv_msghdr), nullptr);
                                                 }));
}

TEST_P(NetDatagramSocketsCmsgSendTest, CmsgLengthOutOfBounds) {
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  cmsghdr cmsg = OrdinaryControlMessage();
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = &cmsg,
      .msg_controllen = cmsg.cmsg_len,
  };
  cmsg.cmsg_len++;

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), -1);
  ASSERT_EQ(errno, EINVAL) << strerror(errno);
}

TEST_P(NetDatagramSocketsCmsgSendTest, ControlBufferSmallerThanCmsgHeader) {
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  cmsghdr cmsg = OrdinaryControlMessage();
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = &cmsg,
      .msg_controllen = sizeof(cmsg) - 1,
  };
  // The control message header would fail basic validation. But because the control buffer length
  // is too small, the control message should be ignored.
  cmsg.cmsg_len = 0;

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), ssize_t(sizeof(send_buf)));
  uint8_t recv_control[CMSG_SPACE(0)];
  ASSERT_NO_FATAL_FAILURE(ReceiveAndCheckMessage(send_buf, sizeof(send_buf), recv_control,
                                                 sizeof(recv_control), [](msghdr& recv_msghdr) {
                                                   EXPECT_EQ(recv_msghdr.msg_controllen, 0u);
                                                   ASSERT_EQ(CMSG_FIRSTHDR(&recv_msghdr), nullptr);
                                                 }));
}

TEST_P(NetDatagramSocketsCmsgSendTest, CmsgLengthSmallerThanCmsgHeader) {
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  cmsghdr cmsg = OrdinaryControlMessage();
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = &cmsg,
      .msg_controllen = cmsg.cmsg_len,
  };
  // It is invalid to have a control message header with a size smaller than itself.
  cmsg.cmsg_len = sizeof(cmsg) - 1;

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), -1);
  ASSERT_EQ(errno, EINVAL) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetDatagramSocketsCmsgSendTests, NetDatagramSocketsCmsgSendTest,
                         testing::Values(AF_INET, AF_INET6),
                         [](const auto info) { return socketDomainToString(info.param); });

class NetDatagramSocketsCmsgTimestampTest : public NetDatagramSocketsCmsgTestBase,
                                            public testing::TestWithParam<sa_family_t> {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(SetUpDatagramSockets(GetParam()));

    // Enable receiving SO_TIMESTAMP control message.
    const int optval = 1;
    ASSERT_EQ(setsockopt(bound().get(), SOL_SOCKET, SO_TIMESTAMP, &optval, sizeof(optval)), 0)
        << strerror(errno);
  }

  void TearDown() override { EXPECT_NO_FATAL_FAILURE(TearDownDatagramSockets()); }
};

TEST_P(NetDatagramSocketsCmsgTimestampTest, RecvCmsg) {
  const std::chrono::duration before = std::chrono::system_clock::now().time_since_epoch();
  char control[CMSG_SPACE(sizeof(timeval)) + 1];
  ASSERT_NO_FATAL_FAILURE(
      SendAndCheckReceivedMessage(control, sizeof(control), [before](msghdr& msghdr) {
        ASSERT_EQ(msghdr.msg_controllen, CMSG_SPACE(sizeof(timeval)));
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
        ASSERT_NE(cmsg, nullptr);
        EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(timeval)));
        EXPECT_EQ(cmsg->cmsg_level, SOL_SOCKET);
        EXPECT_EQ(cmsg->cmsg_type, SO_TIMESTAMP);

        timeval received_tv;
        memcpy(&received_tv, CMSG_DATA(cmsg), sizeof(received_tv));
        const std::chrono::duration received = std::chrono::seconds(received_tv.tv_sec) +
                                               std::chrono::microseconds(received_tv.tv_usec);
        const std::chrono::duration after = std::chrono::system_clock::now().time_since_epoch();
        // It is possible for the clock to 'jump'. To avoid flakiness, do not check the received
        // timestamp if the clock jumped back in time.
        if (before <= after) {
          ASSERT_GE(received, before);
          ASSERT_LE(received, after);
        }

        EXPECT_EQ(CMSG_NXTHDR(&msghdr, cmsg), nullptr);
      }));
}

TEST_P(NetDatagramSocketsCmsgTimestampTest, RecvCmsgUnalignedControlBuffer) {
  const std::chrono::duration before = std::chrono::system_clock::now().time_since_epoch();
  char control[CMSG_SPACE(sizeof(timeval)) + 1];
  // Pass an unaligned control buffer.
  ASSERT_NO_FATAL_FAILURE(
      SendAndCheckReceivedMessage(control + 1, CMSG_LEN(sizeof(timeval)), [before](msghdr& msghdr) {
        ASSERT_EQ(msghdr.msg_controllen, CMSG_SPACE(sizeof(timeval)));
        // Fetch back the control buffer and confirm it is unaligned.
        cmsghdr* unaligned_cmsg = CMSG_FIRSTHDR(&msghdr);
        ASSERT_NE(unaligned_cmsg, nullptr);
        ASSERT_NE(reinterpret_cast<uintptr_t>(unaligned_cmsg) % alignof(cmsghdr), 0u);

        // Do not access the unaligned control header directly as that would be an undefined
        // behavior. Copy the content to a properly aligned variable first.
        char aligned_cmsg[CMSG_SPACE(sizeof(timeval))];
        memcpy(&aligned_cmsg, unaligned_cmsg, sizeof(aligned_cmsg));
        cmsghdr* cmsg = reinterpret_cast<cmsghdr*>(aligned_cmsg);
        EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(timeval)));
        EXPECT_EQ(cmsg->cmsg_level, SOL_SOCKET);
        EXPECT_EQ(cmsg->cmsg_type, SO_TIMESTAMP);

        timeval received_tv;
        memcpy(&received_tv, CMSG_DATA(cmsg), sizeof(received_tv));
        const std::chrono::duration received = std::chrono::seconds(received_tv.tv_sec) +
                                               std::chrono::microseconds(received_tv.tv_usec);
        const std::chrono::duration after = std::chrono::system_clock::now().time_since_epoch();
        // It is possible for the clock to 'jump'. To avoid flakiness, do not check the received
        // timestamp if the clock jumped back in time.
        if (before <= after) {
          ASSERT_GE(received, before);
          ASSERT_LE(received, after);
        }

        // Note: We can't use CMSG_NXTHDR because:
        // * it *must* take the unaligned cmsghdr pointer from the control buffer.
        // * and it may access its members (cmsg_len), which would be an undefined behavior.
        // So we skip the CMSG_NXTHDR assertion that shows that there is no other control message.
      }));
}

INSTANTIATE_TEST_SUITE_P(NetDatagramSocketsCmsgTimestampTests, NetDatagramSocketsCmsgTimestampTest,
                         testing::Values(AF_INET, AF_INET6),
                         [](const auto info) { return socketDomainToString(info.param); });

class NetDatagramSocketsCmsgTimestampNsTest : public NetDatagramSocketsCmsgTestBase,
                                              public testing::TestWithParam<sa_family_t> {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(SetUpDatagramSockets(GetParam()));

    // Enable receiving SO_TIMESTAMPNS control message.
    const int optval = 1;
    ASSERT_EQ(setsockopt(bound().get(), SOL_SOCKET, SO_TIMESTAMPNS, &optval, sizeof(optval)), 0)
        << strerror(errno);
  }

  void TearDown() override { EXPECT_NO_FATAL_FAILURE(TearDownDatagramSockets()); }

  // libc++ implementation of chrono' system_clock uses microseconds, so we can't use it to
  // retrieve the current time for nanosecond timestamp tests.
  // https://github.com/llvm-mirror/libcxx/blob/78d6a7767ed/include/chrono#L1574
  // The high_resolution_clock is also not appropriate, because it is an alias on the
  // steady_clock.
  // https://github.com/llvm-mirror/libcxx/blob/78d6a7767ed/include/chrono#L313
  void TimeSinceEpoch(std::chrono::nanoseconds& out) const {
    struct timespec ts;
    ASSERT_EQ(clock_gettime(CLOCK_REALTIME, &ts), 0) << strerror(errno);
    out = std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
  }
};

TEST_P(NetDatagramSocketsCmsgTimestampNsTest, RecvMsg) {
  std::chrono::nanoseconds before;
  ASSERT_NO_FATAL_FAILURE(TimeSinceEpoch(before));
  char control[CMSG_SPACE(sizeof(timespec)) + 1];
  ASSERT_NO_FATAL_FAILURE(
      SendAndCheckReceivedMessage(control, sizeof(control), [&](msghdr& msghdr) {
        ASSERT_EQ(msghdr.msg_controllen, CMSG_SPACE(sizeof(timeval)));
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
        ASSERT_NE(cmsg, nullptr);
        EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(timespec)));
        EXPECT_EQ(cmsg->cmsg_level, SOL_SOCKET);
        EXPECT_EQ(cmsg->cmsg_type, SO_TIMESTAMPNS);

        timespec received_ts;
        memcpy(&received_ts, CMSG_DATA(cmsg), sizeof(received_ts));
        const std::chrono::duration received = std::chrono::seconds(received_ts.tv_sec) +
                                               std::chrono::nanoseconds(received_ts.tv_nsec);
        std::chrono::nanoseconds after;
        ASSERT_NO_FATAL_FAILURE(TimeSinceEpoch(after));
        // It is possible for the clock to 'jump'. To avoid flakiness, do not check the received
        // timestamp if the clock jumped back in time.
        if (before <= after) {
          ASSERT_GE(received, before);
          ASSERT_LE(received, after);
        }

        EXPECT_EQ(CMSG_NXTHDR(&msghdr, cmsg), nullptr);
      }));
}

TEST_P(NetDatagramSocketsCmsgTimestampNsTest, RecvCmsgUnalignedControlBuffer) {
  std::chrono::nanoseconds before;
  ASSERT_NO_FATAL_FAILURE(TimeSinceEpoch(before));
  char control[CMSG_SPACE(sizeof(timespec)) + 1];
  // Pass an unaligned control buffer.
  ASSERT_NO_FATAL_FAILURE(
      SendAndCheckReceivedMessage(control + 1, CMSG_LEN(sizeof(timespec)), [&](msghdr& msghdr) {
        ASSERT_EQ(msghdr.msg_controllen, CMSG_SPACE(sizeof(timespec)));
        // Fetch back the control buffer and confirm it is unaligned.
        cmsghdr* unaligned_cmsg = CMSG_FIRSTHDR(&msghdr);
        ASSERT_NE(unaligned_cmsg, nullptr);
        ASSERT_NE(reinterpret_cast<uintptr_t>(unaligned_cmsg) % alignof(cmsghdr), 0u);

        // Do not access the unaligned control header directly as that would be an undefined
        // behavior. Copy the content to a properly aligned variable first.
        char aligned_cmsg[CMSG_SPACE(sizeof(timespec))];
        memcpy(&aligned_cmsg, unaligned_cmsg, sizeof(aligned_cmsg));
        cmsghdr* cmsg = reinterpret_cast<cmsghdr*>(aligned_cmsg);
        EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(timespec)));
        EXPECT_EQ(cmsg->cmsg_level, SOL_SOCKET);
        EXPECT_EQ(cmsg->cmsg_type, SO_TIMESTAMPNS);

        timespec received_tv;
        memcpy(&received_tv, CMSG_DATA(cmsg), sizeof(received_tv));
        const std::chrono::duration received = std::chrono::seconds(received_tv.tv_sec) +
                                               std::chrono::nanoseconds(received_tv.tv_nsec);
        std::chrono::nanoseconds after;
        ASSERT_NO_FATAL_FAILURE(TimeSinceEpoch(after));
        // It is possible for the clock to 'jump'. To avoid flakiness, do not check the received
        // timestamp if the clock jumped back in time.
        if (before <= after) {
          ASSERT_GE(received, before);
          ASSERT_LE(received, after);
        }

        // Note: We can't use CMSG_NXTHDR because:
        // * it *must* take the unaligned cmsghdr pointer from the control buffer.
        // * and it may access its members (cmsg_len), which would be an undefined behavior.
        // So we skip the CMSG_NXTHDR assertion that shows that there is no other control message.
      }));
}

INSTANTIATE_TEST_SUITE_P(NetDatagramSocketsCmsgTimestampNsTests,
                         NetDatagramSocketsCmsgTimestampNsTest, testing::Values(AF_INET, AF_INET6),
                         [](const auto info) { return socketDomainToString(info.param); });

class NetDatagramSocketsCmsgIpTosTest : public NetDatagramSocketsCmsgTestBase,
                                        public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(SetUpDatagramSockets(AF_INET));

    // Enable receiving IP_TOS control message.
    const int optval = 1;
    ASSERT_EQ(setsockopt(bound().get(), SOL_IP, IP_RECVTOS, &optval, sizeof(optval)), 0)
        << strerror(errno);
  }

  void TearDown() override {
    if (!IsSkipped()) {
      EXPECT_NO_FATAL_FAILURE(TearDownDatagramSockets());
    }
  }
};

TEST_F(NetDatagramSocketsCmsgIpTosTest, RecvCmsg) {
  constexpr uint8_t tos = 42;
  ASSERT_EQ(setsockopt(connected().get(), SOL_IP, IP_TOS, &tos, sizeof(tos)), 0) << strerror(errno);

  char control[CMSG_SPACE(sizeof(tos)) + 1];
  ASSERT_NO_FATAL_FAILURE(
      SendAndCheckReceivedMessage(control, sizeof(control), [tos](msghdr& msghdr) {
        EXPECT_EQ(msghdr.msg_controllen, CMSG_SPACE(sizeof(tos)));
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
        ASSERT_NE(cmsg, nullptr);
        EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(tos)));
        EXPECT_EQ(cmsg->cmsg_level, SOL_IP);
        EXPECT_EQ(cmsg->cmsg_type, IP_TOS);
        uint8_t recv_tos;
        memcpy(&recv_tos, CMSG_DATA(cmsg), sizeof(recv_tos));
        EXPECT_EQ(recv_tos, tos);
        EXPECT_EQ(CMSG_NXTHDR(&msghdr, cmsg), nullptr);
      }));
}

TEST_F(NetDatagramSocketsCmsgIpTosTest, RecvCmsgBufferTooSmallToBePadded) {
  constexpr uint8_t tos = 42;
  ASSERT_EQ(setsockopt(connected().get(), SOL_IP, IP_TOS, &tos, sizeof(tos)), 0) << strerror(errno);

  // This test is only meaningful if the length of the data is not aligned.
  ASSERT_NE(CMSG_ALIGN(sizeof(tos)), sizeof(tos));
  // Add an extra byte in the control buffer. It will be reported in the msghdr controllen field.
  char control[CMSG_LEN(sizeof(tos)) + 1];
  ASSERT_NO_FATAL_FAILURE(SendAndCheckReceivedMessage(control, sizeof(control), [](msghdr& msghdr) {
    // There is not enough space in the control buffer for it to be padded by CMSG_SPACE. So we
    // expect the size of the input control buffer in controllen instead. It indicates that every
    // bytes from the control buffer were used.
    EXPECT_EQ(msghdr.msg_controllen, CMSG_LEN(sizeof(tos)) + 1);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
    ASSERT_NE(cmsg, nullptr);
    EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(tos)));
    EXPECT_EQ(cmsg->cmsg_level, SOL_IP);
    EXPECT_EQ(cmsg->cmsg_type, IP_TOS);
    EXPECT_EQ(CMSG_NXTHDR(&msghdr, cmsg), nullptr);
  }));
}

TEST_F(NetDatagramSocketsCmsgIpTosTest, SendCmsg) {
  constexpr uint8_t tos = 42;
  char send_buf[] = "hello";
  iovec iovec = {
      .iov_base = send_buf,
      .iov_len = sizeof(send_buf),
  };
  uint8_t send_control[CMSG_SPACE(sizeof(tos))];
  const msghdr send_msghdr = {
      .msg_iov = &iovec,
      .msg_iovlen = 1,
      .msg_control = send_control,
      .msg_controllen = sizeof(send_control),
  };
  cmsghdr* cmsg = CMSG_FIRSTHDR(&send_msghdr);
  ASSERT_NE(cmsg, nullptr);
  cmsg->cmsg_level = SOL_IP;
  cmsg->cmsg_type = IP_TOS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(tos));
  memcpy(CMSG_DATA(cmsg), &tos, sizeof(tos));

  ASSERT_EQ(sendmsg(connected().get(), &send_msghdr, 0), ssize_t(sizeof(send_buf)))
      << strerror(errno);
  char recv_control[CMSG_SPACE(sizeof(tos)) + 1];
  ASSERT_NO_FATAL_FAILURE(ReceiveAndCheckMessage(
      send_buf, sizeof(send_buf), recv_control, sizeof(recv_control), [tos](msghdr& recv_msghdr) {
        EXPECT_EQ(recv_msghdr.msg_controllen, CMSG_SPACE(sizeof(tos)));
        cmsghdr* cmsg = CMSG_FIRSTHDR(&recv_msghdr);
        ASSERT_NE(cmsg, nullptr);
        EXPECT_EQ(cmsg->cmsg_len, CMSG_LEN(sizeof(tos)));
        EXPECT_EQ(cmsg->cmsg_level, SOL_IP);
        EXPECT_EQ(cmsg->cmsg_type, IP_TOS);
        uint8_t recv_tos;
        memcpy(&recv_tos, CMSG_DATA(cmsg), sizeof(recv_tos));
#if defined(__Fuchsia__)
        // TODO(https://fxbug.dev/21106): Support sending SOL_IP -> IP_TOS control message.
        (void)tos;
        constexpr uint8_t kDefaultTOS = 0;
        EXPECT_EQ(recv_tos, kDefaultTOS);
#else
        EXPECT_EQ(recv_tos, tos);
#endif
        EXPECT_EQ(CMSG_NXTHDR(&recv_msghdr, cmsg), nullptr);
      }));
}

}  // namespace
