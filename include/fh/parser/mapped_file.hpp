// Read-only memory mapping of the decompressed ITCH file (POSIX).
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fh {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) fail("open", path);
        struct stat sb {};
        if (::fstat(fd_, &sb) != 0) fail("fstat", path);
        size_ = static_cast<size_t>(sb.st_size);
        if (size_ == 0) return;
        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) fail("mmap", path);
        data_ = static_cast<const uint8_t*>(p);
        // The parser walks front to back exactly once per run.
        ::madvise(p, size_, MADV_SEQUENTIAL);
        ::madvise(p, size_, MADV_WILLNEED);
    }
    ~MappedFile() {
        if (data_) ::munmap(const_cast<uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const uint8_t* data() const { return data_; }
    const uint8_t* end() const { return data_ + size_; }
    size_t size() const { return size_; }

    // Touch every page so the first timed run does not pay for page faults.
    uint64_t prefault() const {
        uint64_t sum = 0;
        for (size_t i = 0; i < size_; i += 4096) sum += data_[i];
        return sum;
    }

private:
    [[noreturn]] void fail(const char* what, const std::string& path) {
        throw std::runtime_error(std::string(what) + "(" + path + "): " + std::strerror(errno));
    }
    int fd_ = -1;
    size_t size_ = 0;
    const uint8_t* data_ = nullptr;
};

}  // namespace fh
