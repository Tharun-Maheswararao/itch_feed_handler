// Kernel UDP receive path (Stage C2): one nonblocking socket per line,
// busy-polled. Linux reads a batch per system call with recvmmsg and takes
// the kernel's receive timestamp (SO_TIMESTAMPING; hardware if asked for and
// the NIC provides it). Other systems loop recvmsg with SO_TIMESTAMP.
// All buffers and message headers are allocated at construction.
#pragma once

#include <memory>
#include <string>

#include "fh/net/rx_path.hpp"
#include "fh/net/udp.hpp"

namespace fh::net {

struct UdpRxConfig {
    std::string line[2];  // local endpoints, e.g. "127.0.0.1:31001" or "239.1.1.1:31001"; "" = unused
    SocketOptions socket{8 << 20, "", 1, true};
    bool hardware_timestamps = false;
    int batch = 32;  // packets per line per poll
};

class UdpRx final : public RxPath {
public:
    explicit UdpRx(const UdpRxConfig& c);
    ~UdpRx() override;

    int poll(RxPacket* out, int max) override;
    void release(int) override {}  // buffers are reused by the next poll
    const char* name() const override;
    uint64_t drops() const override;

    TsSource requested_ts() const { return ts_; }
    int fd(int line) const;
    uint16_t port(int line) const;
    int receive_buffer(int line) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TsSource ts_ = TsSource::UserPoll;
};

}  // namespace fh::net
