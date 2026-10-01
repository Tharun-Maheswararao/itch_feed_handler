// Optional live terminal view: reads the seqlock snapshot on its own thread
// and redraws the top five levels once per interval. It never touches the
// book or the queue, so the pipeline cannot be slowed by it.
#pragma once

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

#include "fh/view/seqlock.hpp"
#include "fh/view/snapshot.hpp"

namespace fh {

std::string render_snapshot(const BookSnapshot& s);

class LiveView {
public:
    LiveView(const Seqlock<BookSnapshot>& source, std::chrono::milliseconds interval, std::string record_path = {},
             bool draw = true);
    ~LiveView();
    void start();
    void stop();  // draws one last frame
    uint64_t frames() const { return frames_; }

private:
    void frame();
    const Seqlock<BookSnapshot>& source_;
    std::chrono::milliseconds interval_;
    std::string record_path_;
    std::ofstream record_;
    bool draw_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    uint64_t frames_ = 0;
    uint64_t last_version_ = 0;
};

}  // namespace fh
