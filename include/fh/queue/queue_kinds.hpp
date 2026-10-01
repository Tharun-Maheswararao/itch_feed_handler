// Runtime selection of a compile-time queue type and capacity.
//
//   with_queue("rigtorp", 65536, [&]<class Q>() { return run_pipeline<Q>(...); });
//
// Every (queue, capacity) pair is its own template instantiation, so the hot
// path has no virtual calls or runtime capacity checks for any of them.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "fh/parser/message.hpp"
#include "fh/queue/industry_queues.hpp"
#include "fh/queue/mutex_queue.hpp"
#include "fh/queue/spsc_queue.hpp"

namespace fh {

inline const std::vector<std::string>& queue_names() {
    static const std::vector<std::string> names = {
        "spsc", "spsc_unaligned", "mutex", "rigtorp",
#if defined(FH_HAVE_BOOST)
        "boost",
#endif
    };
    return names;
}

inline constexpr std::size_t kQueueCapacities[] = {1024, 16384, 65536, 1048576};

namespace detail {
template <template <class, std::size_t> class Q, class F>
decltype(auto) with_capacity(std::size_t capacity, F&& f) {
    switch (capacity) {
        case 1024: return f.template operator()<Q<Msg, 1024>>();
        case 16384: return f.template operator()<Q<Msg, 16384>>();
        case 65536: return f.template operator()<Q<Msg, 65536>>();
        case 1048576: return f.template operator()<Q<Msg, 1048576>>();
        default:
            throw std::invalid_argument("capacity must be one of 1024, 16384, 65536, 1048576");
    }
}
}  // namespace detail

template <class F>
decltype(auto) with_queue(const std::string& name, std::size_t capacity, F&& f) {
    if (name == "spsc") return detail::with_capacity<SpscAligned>(capacity, f);
    if (name == "spsc_unaligned") return detail::with_capacity<SpscUnaligned>(capacity, f);
    if (name == "mutex") return detail::with_capacity<MutexQueue>(capacity, f);
    if (name == "rigtorp") return detail::with_capacity<RigtorpQueue>(capacity, f);
#if defined(FH_HAVE_BOOST)
    if (name == "boost") return detail::with_capacity<BoostQueue>(capacity, f);
#endif
    std::string known;
    for (const auto& n : queue_names()) known += (known.empty() ? "" : ", ") + n;
    throw std::invalid_argument("unknown queue '" + name + "' (available: " + known + ")");
}

// Exact version of each queue implementation, recorded with every result.
inline std::string queue_version(const std::string& name) {
    if (name == "rigtorp") return rigtorp_version();
#if defined(FH_HAVE_BOOST)
    if (name == "boost") return boost_version();
#endif
    if (name == "mutex") return "std::mutex + std::condition_variable";
    return "this repo";
}

}  // namespace fh
