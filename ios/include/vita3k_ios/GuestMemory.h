#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vita3k::ios {

class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();

    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;

    bool reserve(std::uint64_t size, std::string &error);
    bool run_commit_protection_test(std::string &error);
    void release();

    [[nodiscard]] bool ready() const { return base_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] std::size_t host_page_size() const { return host_page_size_; }

private:
    void *base_ = nullptr;
    std::uint64_t size_ = 0;
    std::size_t host_page_size_ = 0;
};

} // namespace vita3k::ios
