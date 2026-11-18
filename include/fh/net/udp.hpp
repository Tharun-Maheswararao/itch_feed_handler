// UDP socket helpers shared by the publisher, the receiver and the recovery
// servers. IPv4 only. Unicast and multicast destinations look the same to
// callers: "host:port", where host is a name, a unicast address or a group.
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>

#include "fh/net/rx_path.hpp"

namespace fh::net {

struct Endpoint {
    sockaddr_in addr{};
    std::string text;  // as given, for messages

    bool multicast() const;
    uint16_t port() const;
};

// "host:port" (host may be empty or "*" for INADDR_ANY). Throws on error.
Endpoint parse_endpoint(const std::string& s);

struct SocketOptions {
    int buffer_bytes = 0;     // SO_SNDBUF / SO_RCVBUF request (0 = system default)
    std::string interface;    // multicast interface address ("" = system default)
    int multicast_ttl = 1;
    bool multicast_loop = true;
};

// Sender connected to `dst`, so send()/sendmsg() need no address and report
// errors such as ECONNREFUSED.
int open_udp_sender(const Endpoint& dst, const SocketOptions& o = {});

// Nonblocking receiver bound to `local`. A multicast address is joined on
// o.interface. Port 0 picks a free port (see bound_port).
int open_udp_receiver(const Endpoint& local, const SocketOptions& o = {});

// Unconnected, nonblocking socket bound to `local` for request/response use
// (retransmit client and server).
int open_udp_socket(const Endpoint& local, int buffer_bytes = 0);

uint16_t bound_port(int fd);
int receive_buffer_bytes(int fd);  // what the kernel actually granted

// Ask for receive timestamps. Returns the best source the socket will give;
// Hardware only if `want_hardware` and the kernel accepts it (the NIC may
// still not stamp, which the receiver detects per packet).
TsSource enable_rx_timestamps(int fd, bool want_hardware);

// Linux: per-socket drop counter delivered with each packet (SO_RXQ_OVFL).
bool enable_drop_counter(int fd);

// Packets the kernel dropped for full UDP receive buffers, system wide
// (Linux /proc/net/snmp RcvbufErrors, macOS udps_fullsock). -1 if unknown.
int64_t system_udp_buffer_drops();

uint64_t realtime_ns() noexcept;

}  // namespace fh::net
