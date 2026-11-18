// SPDX-License-Identifier: GPL-2.0
// XDP program for the Version 2 AF_XDP receive path (Stage C4).
//
// Matches the feed's UDP destination ports (lines A and B, filled in by the
// receiver through the feed_ports map) and redirects those packets to the
// AF_XDP socket bound to the receive queue. Everything else goes on to the
// kernel as usual, so SSH, ARP, the GitHub runner and the recovery thread's
// retransmit traffic keep working on the same interface.
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// UDP destination port (network byte order) -> line (0 = A, 1 = B).
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16);
    __type(key, __u16);
    __type(value, __u8);
} feed_ports SEC(".maps");

// 0: redirected to the socket, 1: passed to the kernel.
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u64);
} counters SEC(".maps");

struct vlan {
    __be16 tci;
    __be16 proto;
};

static __always_inline int pass(void) {
    __u32 k = 1;
    __u64* c = bpf_map_lookup_elem(&counters, &k);
    if (c) *c += 1;
    return XDP_PASS;
}

SEC("xdp")
int xdp_feed(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* end = (void*)(long)ctx->data_end;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > end) return pass();
    __u16 proto = eth->h_proto;
    void* l3 = eth + 1;
    if (proto == bpf_htons(ETH_P_8021Q)) {
        struct vlan* v = l3;
        if ((void*)(v + 1) > end) return pass();
        proto = v->proto;
        l3 = v + 1;
    }
    if (proto != bpf_htons(ETH_P_IP)) return pass();

    struct iphdr* ip = l3;
    if ((void*)(ip + 1) > end || ip->protocol != IPPROTO_UDP) return pass();
    const __u32 ihl = ip->ihl * 4;
    if (ihl < sizeof(*ip)) return pass();
    struct udphdr* udp = (void*)ip + ihl;
    if ((void*)(udp + 1) > end) return pass();

    __u16 port = udp->dest;
    if (!bpf_map_lookup_elem(&feed_ports, &port)) return pass();

    __u32 k = 0;
    __u64* c = bpf_map_lookup_elem(&counters, &k);
    if (c) *c += 1;
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
