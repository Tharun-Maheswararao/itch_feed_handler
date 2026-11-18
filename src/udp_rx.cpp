#include "fh/net/udp_rx.hpp"

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "fh/mold/moldudp64.hpp"

#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#endif

namespace fh::net {

namespace {

constexpr std::size_t kCtrl = 256;  // room for timestamp + drop counter cmsgs

#if defined(__linux__)
uint64_t ts_ns(const timespec& t) {
    return static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(t.tv_nsec);
}
#endif

}  // namespace

struct UdpRx::Impl {
    struct Line {
        int fd = -1;
        std::vector<uint8_t> data;   // batch * kMaxDatagram
        std::vector<uint8_t> ctrl;   // batch * kCtrl
        std::vector<iovec> iov;
        std::vector<msghdr> hdr;
#if defined(__linux__)
        std::vector<mmsghdr> mm;
#endif
        uint64_t drops = 0;  // SO_RXQ_OVFL: cumulative per socket
    };
    Line line[2];
    int batch = 32;
    int next = 0;  // line polled first, alternating for fairness
    bool hw = false;
    bool drop_counter = false;
};

UdpRx::UdpRx(const UdpRxConfig& c) : impl_(std::make_unique<Impl>()) {
    impl_->batch = c.batch;
    impl_->hw = c.hardware_timestamps;
    bool any = false;
    for (int l = 0; l < 2; ++l) {
        if (c.line[l].empty()) continue;
        any = true;
        auto& L = impl_->line[l];
        L.fd = open_udp_receiver(parse_endpoint(c.line[l]), c.socket);
        ts_ = enable_rx_timestamps(L.fd, c.hardware_timestamps);
        impl_->drop_counter = enable_drop_counter(L.fd);
        L.data.resize(static_cast<std::size_t>(c.batch) * mold::kMaxDatagram);
        L.ctrl.resize(static_cast<std::size_t>(c.batch) * kCtrl);
        L.iov.resize(c.batch);
        L.hdr.resize(c.batch);
        for (int i = 0; i < c.batch; ++i) {
            L.iov[i] = {L.data.data() + static_cast<std::size_t>(i) * mold::kMaxDatagram, mold::kMaxDatagram};
            L.hdr[i] = msghdr{};
            L.hdr[i].msg_iov = &L.iov[i];
            L.hdr[i].msg_iovlen = 1;
            L.hdr[i].msg_control = L.ctrl.data() + static_cast<std::size_t>(i) * kCtrl;
            L.hdr[i].msg_controllen = kCtrl;
        }
#if defined(__linux__)
        L.mm.resize(c.batch);
        for (int i = 0; i < c.batch; ++i) L.mm[i].msg_hdr = L.hdr[i];
#endif
    }
    if (!any) throw std::runtime_error("receiver: no line configured");
}

UdpRx::~UdpRx() {
    for (auto& L : impl_->line)
        if (L.fd >= 0) close(L.fd);
}

const char* UdpRx::name() const {
#if defined(__linux__)
    return "kernel UDP sockets (recvmmsg)";
#else
    return "kernel UDP sockets (recvmsg)";
#endif
}

uint64_t UdpRx::drops() const { return impl_->line[0].drops + impl_->line[1].drops; }
int UdpRx::fd(int line) const { return impl_->line[line].fd; }
uint16_t UdpRx::port(int line) const { return impl_->line[line].fd >= 0 ? bound_port(impl_->line[line].fd) : 0; }
int UdpRx::receive_buffer(int line) const {
    return impl_->line[line].fd >= 0 ? receive_buffer_bytes(impl_->line[line].fd) : 0;
}

int UdpRx::poll(RxPacket* out, int max) {
    int n = 0;
    for (int k = 0; k < 2 && n < max; ++k) {
        const int l = (impl_->next + k) & 1;
        auto& L = impl_->line[l];
        if (L.fd < 0) continue;
        const int want = std::min(impl_->batch, max - n);
#if defined(__linux__)
        for (int i = 0; i < want; ++i) {
            L.mm[i].msg_hdr.msg_controllen = kCtrl;
            L.mm[i].msg_len = 0;
        }
        const int got = recvmmsg(L.fd, L.mm.data(), static_cast<unsigned>(want), MSG_DONTWAIT, nullptr);
        if (got <= 0) continue;
        const uint64_t now = realtime_ns();  // fallback when a packet carries no stamp
        for (int i = 0; i < got; ++i) {
            msghdr& h = L.mm[i].msg_hdr;
            RxPacket& p = out[n++];
            p.data = static_cast<const uint8_t*>(L.iov[i].iov_base);
            p.len = L.mm[i].msg_len;
            p.line = static_cast<uint8_t>(l);
            p.rx_ns = now;
            p.ts_source = TsSource::UserPoll;
            for (cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
                if (c->cmsg_level != SOL_SOCKET) continue;
                if (c->cmsg_type == SO_TIMESTAMPING) {
                    timespec t[3];
                    std::memcpy(t, CMSG_DATA(c), sizeof t);
                    if (impl_->hw && (t[2].tv_sec || t[2].tv_nsec)) {
                        p.rx_ns = ts_ns(t[2]);
                        p.ts_source = TsSource::Hardware;
                    } else if (t[0].tv_sec || t[0].tv_nsec) {
                        p.rx_ns = ts_ns(t[0]);
                        p.ts_source = TsSource::KernelSoftware;
                    }
                } else if (c->cmsg_type == SO_RXQ_OVFL) {
                    uint32_t d;
                    std::memcpy(&d, CMSG_DATA(c), sizeof d);
                    L.drops = d;
                }
            }
        }
#else
        const uint64_t now = realtime_ns();
        for (int i = 0; i < want; ++i) {
            msghdr& h = L.hdr[i];
            h.msg_controllen = kCtrl;
            const ssize_t r = recvmsg(L.fd, &h, MSG_DONTWAIT);
            if (r <= 0) break;
            RxPacket& p = out[n++];
            p.data = static_cast<const uint8_t*>(L.iov[i].iov_base);
            p.len = static_cast<uint32_t>(r);
            p.line = static_cast<uint8_t>(l);
            p.rx_ns = now;
            p.ts_source = TsSource::UserPoll;
            for (cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) {
                    timeval tv;
                    std::memcpy(&tv, CMSG_DATA(c), sizeof tv);
                    p.rx_ns = static_cast<uint64_t>(tv.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(tv.tv_usec) * 1000ull;
                    p.ts_source = TsSource::KernelSoftware;
                }
            }
        }
#endif
    }
    impl_->next ^= 1;
    return n;
}

}  // namespace fh::net
