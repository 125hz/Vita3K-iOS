#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vita3k::ios {

class GuestMemory;

struct GuestHeapStats {
    std::size_t allocation_count = 0;
    std::size_t free_count = 0;
    std::size_t realloc_count = 0;
    std::uint64_t live_bytes = 0;
    std::uint64_t peak_bytes = 0;
};

class GuestHeap {
public:
    bool initialize(GuestMemory &memory,
        std::uint32_t base,
        std::uint32_t size,
        std::string &error);
    std::uint32_t allocate(std::uint32_t size,
        std::uint32_t alignment,
        bool zero,
        std::string &error);
    std::uint32_t reallocate(std::uint32_t address,
        std::uint32_t size,
        std::string &error);
    bool free(std::uint32_t address, std::string &error);
    bool usable_size(std::uint32_t address, std::uint32_t &size, std::string &error) const;

    [[nodiscard]] bool ready() const { return memory_ != nullptr; }
    [[nodiscard]] std::uint32_t base() const { return base_; }
    [[nodiscard]] std::uint32_t size() const { return size_; }
    [[nodiscard]] const GuestHeapStats &stats() const { return stats_; }

private:
    struct Block {
        std::uint32_t address = 0;
        std::uint32_t size = 0;
        bool allocated = false;
    };

    void coalesce();

    GuestMemory *memory_ = nullptr;
    std::uint32_t base_ = 0;
    std::uint32_t size_ = 0;
    std::vector<Block> blocks_;
    GuestHeapStats stats_;
};

} // namespace vita3k::ios
