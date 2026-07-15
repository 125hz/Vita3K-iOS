#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vita3k::ios {

struct GuestSegmentMapping {
    std::uint32_t guest_address;
    std::span<const std::uint8_t> file_data;
    std::uint32_t memory_size;
    std::uint32_t guest_flags;
};

class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();

    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;

    bool reserve(std::uint64_t size, std::string &error);
    bool run_commit_protection_test(std::string &error);
    bool map_segment(std::uint32_t guest_address,
        std::span<const std::uint8_t> file_data,
        std::uint32_t memory_size,
        std::uint32_t guest_flags,
        std::string &error);
    bool map_segments(std::span<const GuestSegmentMapping> segments, std::string &error);
    bool map_dynamic_segment(std::uint32_t guest_address,
        std::uint32_t memory_size,
        std::uint32_t guest_flags,
        std::string &error);
    bool read(std::uint32_t guest_address,
        std::span<std::uint8_t> output,
        std::string &error) const;
    bool write(std::uint32_t guest_address,
        std::span<const std::uint8_t> input,
        std::string &error);
    bool unmap_segment(std::uint32_t guest_address,
        std::uint32_t memory_size,
        std::string &error);
    bool unmap_all_segments(std::string &error);
    void release();

    [[nodiscard]] bool ready() const { return base_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] std::size_t host_page_size() const { return host_page_size_; }
    [[nodiscard]] std::size_t mapped_segment_count() const { return mapped_segment_ranges_.size(); }

private:
    struct MappedPageRange {
        std::uint64_t page_start;
        std::uint64_t page_size;
        std::uint32_t guest_flags;
    };

    struct MappedSegmentRange {
        std::uint32_t guest_start;
        std::uint32_t memory_size;
        std::uint32_t guest_flags;
    };

    void *base_ = nullptr;
    std::uint64_t size_ = 0;
    std::size_t host_page_size_ = 0;
    std::vector<MappedPageRange> mapped_page_ranges_;
    std::vector<MappedSegmentRange> mapped_segment_ranges_;
};

} // namespace vita3k::ios
