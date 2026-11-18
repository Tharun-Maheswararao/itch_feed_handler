#include "fh/net/udp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(__linux__)
#include <linux/net_tstamp.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#endif

namespace fh::net {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

void set_int(int fd, int level, int opt, int v, const char* name) {
    if (setsockopt(fd, level, opt, &v, sizeof v) != 0) fail(std::string("setsockopt ") + name);
}

void set_nonblocking(int fd) {
    const int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) fail("fcntl O_NONBLOCK");
}

in_addr interface_addr(const std::string& iface) {
    in_addr a{};
    a.s_addr = htonl(INADDR_ANY);
    if (!iface.empty() && inet_pton(AF_INET, iface.c_str(), &a) != 1)
        throw std::runtime_error("multicast interface must be an IPv4 address: " + iface);
    return a;
}

// Buffer requests above the system limit fail on macOS and are silently
// capped on Linux. Step down until one is accepted.
void request_buffer(int fd, int opt, int bytes) {
    for (int b = bytes; b >= 64 * 1024; b /= 2)
        if (setsockopt(fd, SOL_SOCKET, opt, &b, sizeof b) == 0) return;
}

}  // namespace

bool Endpoint::multicast() const { return IN_MULTICAST(ntohl(addr.sin_addr.s_addr)); }
uint16_t Endpoint::port() const { return ntohs(addr.sin_port); }

Endpoint parse_endpoint(const std::string& s) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("endpoint must be host:port: " + s);
    const std::string host = s.substr(0, colon);
    const long port = std::strtol(s.c_str() + colon + 1, nullptr, 10);
    if (port < 0 || port > 65535) throw std::runtime_error("bad port in " + s);
    Endpoint e;
    e.text = s;
    e.addr.sin_family = AF_INET;
    e.addr.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "*") {
        e.addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host.c_str(), &e.addr.sin_addr) != 1) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
            throw std::runtime_error("cannot resolve " + host);
        e.addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    return e;
}

int open_udp_sender(const Endpoint& dst, const SocketOptions& o) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) fail("socket");
    if (o.buffer_bytes > 0) request_buffer(fd, SO_SNDBUF, o.buffer_bytes);
    if (dst.multicast()) {
        const unsigned char ttl = static_cast<unsigned char>(o.multicast_ttl);
        const unsigned char loop = o.multicast_loop ? 1 : 0;
        const in_addr ifa = interface_addr(o.interface);
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) != 0) fail("IP_MULTICAST_TTL");
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) != 0) fail("IP_MULTICAST_LOOP");
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa) != 0) fail("IP_MULTICAST_IF");
    }
    if (connect(fd, reinterpret_cast<const sockaddr*>(&dst.addr), sizeof dst.addr) != 0) fail("connect " + dst.text);
    return fd;
}

int open_udp_receiver(const Endpoint& local, const SocketOptions& o) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) fail("socket");
    set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");
#ifdef SO_REUSEPORT
    if (local.multicast()) set_int(fd, SOL_SOCKET, SO_REUSEPORT, 1, "SO_REUSEPORT");
#endif
    if (o.buffer_bytes > 0) request_buffer(fd, SO_RCVBUF, o.buffer_bytes);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&local.addr), sizeof local.addr) != 0) fail("bind " + local.text);
    if (local.multicast()) {
        ip_mreq m{};
        m.imr_multiaddr = local.addr.sin_addr;
        m.imr_interface = interface_addr(o.interface);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0) fail("IP_ADD_MEMBERSHIP " + local.text);
    }
    set_nonblocking(fd);
    return fd;
}

int open_udp_socket(const Endpoint& local, int buffer_bytes) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) fail("socket");
    if (buffer_bytes > 0) {
        request_buffer(fd, SO_RCVBUF, buffer_bytes);
        request_buffer(fd, SO_SNDBUF, buffer_bytes);
    }
    if (bind(fd, reinterpret_cast<const sockaddr*>(&local.addr), sizeof local.addr) != 0) fail("bind " + local.text);
    set_nonblocking(fd);
    return fd;
}

uint16_t bound_port(int fd) {
    sockaddr_in a{};
    socklen_t n = sizeof a;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) != 0) fail("getsockname");
    return ntohs(a.sin_port);
}

int receive_buffer_bytes(int fd) {
    int v = 0;
    socklen_t n = sizeof v;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &n);
    return v;
}

TsSource enable_rx_timestamps(int fd, bool want_hardware) {
#if defined(__linux__)
    unsigned flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (want_hardware) flags |= SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof flags) == 0)
        return want_hardware ? TsSource::Hardware : TsSource::KernelSoftware;
    return TsSource::UserPoll;
#else
    (void)want_hardware;
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof on) == 0) return TsSource::KernelSoftware;
    return TsSource::UserPoll;
#endif
}

bool enable_drop_counter(int fd) {
#if defined(__linux__)
    const int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof on) == 0;
#else
    (void)fd;
    return false;
#endif
}

int64_t system_udp_buffer_drops() {
#if defined(__linux__)
    std::ifstream in("/proc/net/snmp");
    std::string header, values;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("Udp:", 0) != 0) continue;
        if (header.empty()) header = line;
        else { values = line; break; }
    }
    std::istringstream h(header), v(values);
    for (std::string name, val; h >> name && v >> val;)
        if (name == "RcvbufErrors") return std::stoll(val);
    return -1;
#elif defined(__APPLE__)
    udpstat st{};
    size_t n = sizeof st;
    if (sysctlbyname("net.inet.udp.stats", &st, &n, nullptr, 0) != 0) return -1;
    return static_cast<int64_t>(st.udps_fullsock);
#else
    return -1;
#endif
}

uint64_t realtime_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace fh::net
