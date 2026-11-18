#include "fh/net/xdp_rx.hpp"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "fh/net/frame.hpp"
#include "fh/net/udp.hpp"

#ifndef FH_XDP_OBJECT
#define FH_XDP_OBJECT "xdp_feed.bpf.o"
#endif

namespace fh::net {

namespace {
[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error("AF_XDP: " + what + ": " + std::strerror(err < 0 ? -err : err));
}
}  // namespace

struct XdpRx::Impl {
    XdpRxConfig cfg;
    unsigned ifindex = 0;
    xdp_program* prog = nullptr;
    xdp_attach_mode mode = XDP_MODE_SKB;
    void* umem_area = nullptr;
    std::size_t umem_size = 0;
    xsk_umem* umem = nullptr;
    xsk_ring_prod fill{};
    xsk_ring_cons comp{};
    xsk_ring_cons rx{};
    xsk_socket* xsk = nullptr;
    int counters_fd = -1;
    bool zero_copy = false;
    std::string name;
    std::vector<int> group_fds;  // plain sockets that only hold the multicast memberships

    // Descriptors peeked by the last poll, all given back by release().
    uint32_t rx_idx = 0, peeked = 0;
    std::vector<uint64_t> held;
    uint64_t non_feed = 0;

    ~Impl() {
        if (xsk) xsk_socket__delete(xsk);
        if (umem) xsk_umem__delete(umem);
        if (prog) {
            xdp_program__detach(prog, ifindex, mode, 0);
            xdp_program__close(prog);
        }
        if (umem_area) munmap(umem_area, umem_size);
        for (int fd : group_fds) close(fd);
    }
};

XdpRx::XdpRx(const XdpRxConfig& c) : impl_(std::make_unique<Impl>()) {
    Impl& I = *impl_;
    I.cfg = c;
    if (I.cfg.bpf_object.empty()) I.cfg.bpf_object = FH_XDP_OBJECT;
    I.ifindex = if_nametoindex(c.ifname.c_str());
    if (!I.ifindex) fail("no interface " + c.ifname, errno);
    I.mode = c.native ? XDP_MODE_NATIVE : XDP_MODE_SKB;

    // 1. Our XDP program, attached to the interface.
    I.prog = xdp_program__open_file(I.cfg.bpf_object.c_str(), "xdp", nullptr);
    if (long e = libxdp_get_error(I.prog)) {
        I.prog = nullptr;
        fail("open " + I.cfg.bpf_object, static_cast<int>(e));
    }
    if (int e = xdp_program__attach(I.prog, I.ifindex, I.mode, 0)) fail("attach to " + c.ifname, e);
    bpf_object* obj = xdp_program__bpf_obj(I.prog);
    const int ports_fd = bpf_object__find_map_fd_by_name(obj, "feed_ports");
    const int xsks_fd = bpf_object__find_map_fd_by_name(obj, "xsks_map");
    I.counters_fd = bpf_object__find_map_fd_by_name(obj, "counters");
    if (ports_fd < 0 || xsks_fd < 0) fail("maps missing in " + I.cfg.bpf_object, ENOENT);
    for (uint8_t l = 0; l < 2; ++l) {
        if (!c.port[l]) continue;
        const uint16_t key = htons(c.port[l]);
        if (int e = bpf_map_update_elem(ports_fd, &key, &l, BPF_ANY)) fail("feed_ports update", e);
    }

    // 2. UMEM: one region, every frame handed to the fill ring up front.
    I.umem_size = std::size_t{c.frames} * c.frame_size;
    I.umem_area = mmap(nullptr, I.umem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (I.umem_area == MAP_FAILED) {
        I.umem_area = nullptr;
        fail("mmap UMEM", errno);
    }
    xsk_umem_config ucfg{};
    ucfg.fill_size = c.frames;
    ucfg.comp_size = 64;
    ucfg.frame_size = c.frame_size;
    ucfg.frame_headroom = 0;
    if (int e = xsk_umem__create(&I.umem, I.umem_area, I.umem_size, &I.fill, &I.comp, &ucfg)) fail("umem create", e);

    // 3. The socket on one queue, receive only. Zero-copy first if asked.
    xsk_socket_config scfg{};
    scfg.rx_size = c.ring;
    scfg.tx_size = 0;
    scfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
    scfg.xdp_flags = c.native ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
    int err = -1;
    if (c.zero_copy) {
        scfg.bind_flags = XDP_ZEROCOPY | XDP_USE_NEED_WAKEUP;
        err = xsk_socket__create(&I.xsk, c.ifname.c_str(), c.queue, I.umem, &I.rx, nullptr, &scfg);
        I.zero_copy = err == 0;
    }
    if (err) {
        scfg.bind_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;
        err = xsk_socket__create(&I.xsk, c.ifname.c_str(), c.queue, I.umem, &I.rx, nullptr, &scfg);
    }
    if (err) fail("socket on " + c.ifname + " queue " + std::to_string(c.queue), err);
    if (int e = xsk_socket__update_xskmap(I.xsk, xsks_fd)) fail("xsks_map update", e);

    uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&I.fill, c.frames, &idx) != c.frames) fail("fill ring reserve", ENOMEM);
    for (uint32_t i = 0; i < c.frames; ++i) *xsk_ring_prod__fill_addr(&I.fill, idx++) = uint64_t{i} * c.frame_size;
    xsk_ring_prod__submit(&I.fill, c.frames);
    I.held.resize(c.ring);

    // 4. Multicast: the frames still have to get past the NIC's address filter.
    for (int l = 0; l < 2; ++l) {
        if (!c.group[l]) continue;
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        ip_mreqn m{};
        m.imr_multiaddr.s_addr = htonl(c.group[l]);
        m.imr_ifindex = static_cast<int>(I.ifindex);
        if (fd >= 0 && setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0) I.group_fds.push_back(fd);
        else if (fd >= 0) close(fd);
    }

    I.name = std::string("AF_XDP (") + (I.zero_copy ? "zero-copy" : "copy mode") + ", " +
             (c.native ? "native" : "generic") + " XDP on " + c.ifname + " queue " + std::to_string(c.queue) + ")";
}

XdpRx::~XdpRx() = default;

const char* XdpRx::name() const { return impl_->name.c_str(); }

int XdpRx::poll(RxPacket* out, int max) {
    Impl& I = *impl_;
    if (I.peeked) release(0);  // a batch that held only non-feed frames
    if (xsk_ring_prod__needs_wakeup(&I.fill))
        recvfrom(xsk_socket__fd(I.xsk), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    const uint32_t want = std::min<uint32_t>(static_cast<uint32_t>(max), static_cast<uint32_t>(I.held.size()));
    I.peeked = xsk_ring_cons__peek(&I.rx, want, &I.rx_idx);
    if (I.peeked == 0) return 0;
    const uint64_t now = realtime_ns();
    int n = 0;
    for (uint32_t i = 0; i < I.peeked; ++i) {
        const xdp_desc* d = xsk_ring_cons__rx_desc(&I.rx, I.rx_idx + i);
        I.held[i] = d->addr;
        const auto* frame = static_cast<const uint8_t*>(xsk_umem__get_data(I.umem_area, xsk_umem__add_offset_to_addr(d->addr)));
        UdpView u;
        int line = -1;
        if (parse_udp_frame(frame, d->len, u)) {
            if (u.dst_port == I.cfg.port[0]) line = 0;
            else if (u.dst_port == I.cfg.port[1]) line = 1;
        }
        if (line < 0) {
            ++I.non_feed;
            continue;
        }
        out[n++] = RxPacket{u.payload, u.len, static_cast<uint8_t>(line), now, TsSource::UserPoll};
    }
    return n;
}

// Every peeked descriptor goes back, whatever poll() returned for it.
void XdpRx::release(int) {
    Impl& I = *impl_;
    if (I.peeked == 0) return;
    uint32_t idx = 0;
    while (xsk_ring_prod__reserve(&I.fill, I.peeked, &idx) != I.peeked) {
    }
    for (uint32_t i = 0; i < I.peeked; ++i)
        *xsk_ring_prod__fill_addr(&I.fill, idx++) = xsk_umem__extract_addr(I.held[i]);
    xsk_ring_prod__submit(&I.fill, I.peeked);
    xsk_ring_cons__release(&I.rx, I.peeked);
    I.peeked = 0;
}

uint64_t XdpRx::drops() const {
    xdp_statistics s{};
    socklen_t len = sizeof s;
    if (getsockopt(xsk_socket__fd(impl_->xsk), SOL_XDP, XDP_STATISTICS, &s, &len) != 0) return 0;
    return s.rx_dropped + s.rx_ring_full + s.rx_fill_ring_empty_descs;
}

uint64_t XdpRx::redirected() const {
    if (impl_->counters_fd < 0) return 0;
    const int cpus = libbpf_num_possible_cpus();
    if (cpus <= 0) return 0;
    std::vector<uint64_t> v(static_cast<std::size_t>(cpus));
    const uint32_t key = 0;
    if (bpf_map_lookup_elem(impl_->counters_fd, &key, v.data()) != 0) return 0;
    uint64_t sum = 0;
    for (uint64_t x : v) sum += x;
    return sum;
}

uint64_t XdpRx::non_feed() const { return impl_->non_feed; }

}  // namespace fh::net
