// The receive interface every transport implements (kernel sockets, AF_XDP,
// DPDK). Everything after it (arbitration, recovery, the book) is shared, so
// the latency comparison between transports is fair.
#pragma once

#include <cstdint>

namespace fh::net {

// Where a packet's arrival time came from. Results always say which one.
//   Hardware        the NIC's clock, read through SO_TIMESTAMPING
//   KernelSoftware  the kernel's receive stamp (later than the NIC's)
//   UserPoll        read in user space right after the packet was found
//                   (later still: includes the poll loop's own delay)
enum class TsSource : uint8_t { Hardware, KernelSoftware, UserPoll };

inline const char* ts_source_name(TsSource s) {
    switch (s) {
        case TsSource::Hardware: return "hardware (NIC) receive timestamp";
        case TsSource::KernelSoftware: return "kernel software receive timestamp";
        case TsSource::UserPoll: return "user-space poll timestamp";
    }
    return "?";
}

struct RxPacket {
    const uint8_t* data;  // MoldUDP64 packet (UDP payload)
    uint32_t len;
    uint8_t line;         // 0 = A, 1 = B
    uint64_t rx_ns;       // CLOCK_REALTIME nanoseconds, the clock the book thread also reads
    TsSource ts_source;
};

class RxPath {
public:
    virtual ~RxPath() = default;
    // Up to `max` packets; 0 if none is waiting. Never blocks, never allocates.
    // Packets stay valid until release().
    virtual int poll(RxPacket* out, int max) = 0;
    // Give the buffers of the last `count` polled packets back.
    virtual void release(int count) = 0;
    virtual const char* name() const = 0;
    // Packets the kernel or NIC dropped before this path saw them, if known.
    virtual uint64_t drops() const { return 0; }
};

}  // namespace fh::net
