#include <vita3k_ios/GuestMemory.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace vita3k::ios {
namespace {

std::string host_error_message() {
#ifdef _WIN32
    return std::system_category().message(static_cast<int>(GetLastError()));
#else
    return std::generic_category().message(errno);
#endif
}

constexpr std::uint32_t guest_flag_execute = 1;
constexpr std::uint32_t guest_flag_write = 2;
constexpr std::uint32_t guest_flag_read = 4;
constexpr std::uint32_t supported_guest_flags =
    guest_flag_execute | guest_flag_write | guest_flag_read;

} // namespace

GuestMemory::~GuestMemory() {
    release();
}

bool GuestMemory::reserve(std::uint64_t size, std::string &error) {
    release();
    if (size == 0 || size > std::numeric_limits<std::size_t>::max()) {
        error = "Guest-memory size is not representable on this host.";
        return false;
    }

#ifdef _WIN32
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    host_page_size_ = system_info.dwPageSize;
    base_ = VirtualAlloc(nullptr, static_cast<std::size_t>(size), MEM_RESERVE, PAGE_NOACCESS);
#else
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        error = "Could not determine the host page size.";
        return false;
    }
    host_page_size_ = static_cast<std::size_t>(page_size);
    base_ = mmap(nullptr, static_cast<std::size_t>(size), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
    }
#endif

    if (base_ == nullptr) {
        error = "Could not reserve guest address space: " + host_error_message();
        host_page_size_ = 0;
        return false;
    }

    size_ = size;
    return true;
}

bool GuestMemory::run_commit_protection_test(std::string &error) {
    if (!ready() || host_page_size_ < sizeof(std::uint64_t)) {
        error = "Guest memory is not reserved or the host page size is invalid.";
        return false;
    }

#ifdef _WIN32
    void *committed = VirtualAlloc(base_, host_page_size_, MEM_COMMIT, PAGE_READWRITE);
    if (committed != base_) {
        error = "Could not commit a guest-memory test page: " + host_error_message();
        return false;
    }
#else
    if (mprotect(base_, host_page_size_, PROT_READ | PROT_WRITE) != 0) {
        error = "Could not make a guest-memory test page writable: " + host_error_message();
        return false;
    }
#endif

    constexpr std::uint64_t marker = 0x56495441334B694FULL;
    std::memcpy(base_, &marker, sizeof(marker));
    std::uint64_t observed = 0;
    std::memcpy(&observed, base_, sizeof(observed));
    if (observed != marker) {
        error = "Guest-memory test page did not preserve written data.";
        return false;
    }

#ifdef _WIN32
    if (!VirtualFree(base_, host_page_size_, MEM_DECOMMIT)) {
        error = "Could not decommit the guest-memory test page: " + host_error_message();
        return false;
    }
#else
    if (mprotect(base_, host_page_size_, PROT_NONE) != 0) {
        error = "Could not protect the guest-memory test page: " + host_error_message();
        return false;
    }
    (void)madvise(base_, host_page_size_, MADV_DONTNEED);
#endif

    return true;
}

bool GuestMemory::map_segment(std::uint32_t guest_address,
    std::span<const std::uint8_t> file_data,
    std::uint32_t memory_size,
    std::uint32_t guest_flags,
    std::string &error) {
    const std::array segment{
        GuestSegmentMapping{
            .guest_address = guest_address,
            .file_data = file_data,
            .memory_size = memory_size,
            .guest_flags = guest_flags
        }
    };
    return map_segments(segment, error);
}

bool GuestMemory::map_segments(std::span<const GuestSegmentMapping> segments, std::string &error) {
    if (!ready() || host_page_size_ == 0) {
        error = "Guest memory is not reserved.";
        return false;
    }
    if (segments.empty()) {
        error = "No guest segments were supplied.";
        return false;
    }
    if (!mapped_page_ranges_.empty() || !mapped_segment_ranges_.empty()) {
        error = "A guest executable is already mapped.";
        return false;
    }

    struct PlannedSegment {
        const GuestSegmentMapping *source;
        std::uint64_t start;
        std::uint64_t end;
        std::uint64_t page_start;
        std::uint64_t page_end;
    };

    const auto page_size = static_cast<std::uint64_t>(host_page_size_);
    std::vector<PlannedSegment> planned;
    std::vector<std::uint64_t> boundaries;
    planned.reserve(segments.size());
    boundaries.reserve(segments.size() * 2);

    for (const auto &segment : segments) {
        if (segment.memory_size == 0 || segment.file_data.size() > segment.memory_size) {
            error = "A guest segment has invalid file and memory sizes.";
            return false;
        }
        if ((segment.guest_flags & ~supported_guest_flags) != 0) {
            error = "A guest segment contains unsupported permission flags.";
            return false;
        }

        const auto start = static_cast<std::uint64_t>(segment.guest_address);
        const auto end = start + segment.memory_size;
        if (end > size_) {
            error = "A guest segment exceeds the reserved address space.";
            return false;
        }
        const auto page_start = (start / page_size) * page_size;
        const auto page_end = ((end + page_size - 1) / page_size) * page_size;
        if (page_end > size_ || page_end <= page_start) {
            error = "An aligned guest segment range is invalid.";
            return false;
        }

        planned.push_back({
            .source = &segment,
            .start = start,
            .end = end,
            .page_start = page_start,
            .page_end = page_end
        });
        boundaries.push_back(page_start);
        boundaries.push_back(page_end);
    }

    std::ranges::sort(planned, {}, &PlannedSegment::start);
    for (std::size_t index = 1; index < planned.size(); ++index) {
        if (planned[index].start < planned[index - 1].end) {
            error = "Guest segments overlap in byte address space.";
            return false;
        }
    }

    std::ranges::sort(boundaries);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    std::vector<MappedPageRange> page_ranges;
    for (std::size_t index = 1; index < boundaries.size(); ++index) {
        const auto range_start = boundaries[index - 1];
        const auto range_end = boundaries[index];
        bool covered = false;
        std::uint32_t merged_flags = 0;
        for (const auto &segment : planned) {
            if (range_start < segment.page_end && segment.page_start < range_end) {
                covered = true;
                merged_flags |= segment.source->guest_flags;
            }
        }
        if (!covered) {
            continue;
        }
        if (!page_ranges.empty() &&
            page_ranges.back().page_start + page_ranges.back().page_size == range_start &&
            page_ranges.back().guest_flags == merged_flags) {
            page_ranges.back().page_size += range_end - range_start;
        } else {
            page_ranges.push_back({
                .page_start = range_start,
                .page_size = range_end - range_start,
                .guest_flags = merged_flags
            });
        }
    }

    auto rollback = [this](const std::vector<MappedPageRange> &ranges) {
        for (const auto &range : ranges) {
            auto *pointer = static_cast<std::uint8_t *>(base_) + range.page_start;
#ifdef _WIN32
            (void)VirtualFree(pointer, static_cast<std::size_t>(range.page_size), MEM_DECOMMIT);
#else
            (void)mprotect(pointer, static_cast<std::size_t>(range.page_size), PROT_NONE);
            (void)madvise(pointer, static_cast<std::size_t>(range.page_size), MADV_DONTNEED);
#endif
        }
    };

    std::vector<MappedPageRange> committed;
    committed.reserve(page_ranges.size());
    for (const auto &range : page_ranges) {
        auto *pointer = static_cast<std::uint8_t *>(base_) + range.page_start;
#ifdef _WIN32
        void *result = VirtualAlloc(pointer, static_cast<std::size_t>(range.page_size),
            MEM_COMMIT, PAGE_READWRITE);
        if (result != pointer) {
            error = "Could not commit guest segment pages: " + host_error_message();
            rollback(committed);
            return false;
        }
#else
        if (mprotect(pointer, static_cast<std::size_t>(range.page_size), PROT_READ | PROT_WRITE) != 0) {
            error = "Could not make guest segment pages writable: " + host_error_message();
            rollback(committed);
            return false;
        }
#endif
        committed.push_back(range);
    }

    for (const auto &segment : planned) {
        auto *pointer = static_cast<std::uint8_t *>(base_) + segment.start;
        if (!segment.source->file_data.empty()) {
            std::memcpy(pointer, segment.source->file_data.data(), segment.source->file_data.size());
        }
        std::memset(pointer + segment.source->file_data.size(), 0,
            segment.source->memory_size - segment.source->file_data.size());
    }

    for (const auto &range : page_ranges) {
        auto *pointer = static_cast<std::uint8_t *>(base_) + range.page_start;
#ifdef _WIN32
        const DWORD final_protection = (range.guest_flags & guest_flag_write) != 0
            ? PAGE_READWRITE
            : ((range.guest_flags & (guest_flag_read | guest_flag_execute)) != 0
                    ? PAGE_READONLY
                    : PAGE_NOACCESS);
        DWORD previous_protection = 0;
        if (!VirtualProtect(pointer, static_cast<std::size_t>(range.page_size),
                final_protection, &previous_protection)) {
            error = "Could not apply final guest segment protection: " + host_error_message();
            rollback(committed);
            return false;
        }
#else
        int final_protection = PROT_NONE;
        if ((range.guest_flags & guest_flag_write) != 0) {
            final_protection = PROT_READ | PROT_WRITE;
        } else if ((range.guest_flags & (guest_flag_read | guest_flag_execute)) != 0) {
            // Guest execute permission does not require host execute permission.
            // The CPU backend reads guest bytes and emits code in a separate JIT area.
            final_protection = PROT_READ;
        }
        if (mprotect(pointer, static_cast<std::size_t>(range.page_size), final_protection) != 0) {
            error = "Could not apply final guest segment protection: " + host_error_message();
            rollback(committed);
            return false;
        }
#endif
    }

    mapped_page_ranges_ = std::move(page_ranges);
    mapped_segment_ranges_.reserve(planned.size());
    for (const auto &segment : planned) {
        mapped_segment_ranges_.push_back({
            .guest_start = segment.source->guest_address,
            .memory_size = segment.source->memory_size,
            .guest_flags = segment.source->guest_flags
        });
    }
    return true;
}

bool GuestMemory::read(std::uint32_t guest_address,
    std::span<std::uint8_t> output,
    std::string &error) const {
    if (output.empty()) {
        return true;
    }
    const auto read_start = static_cast<std::uint64_t>(guest_address);
    const auto read_end = read_start + output.size();
    const auto mapping = std::find_if(mapped_segment_ranges_.begin(), mapped_segment_ranges_.end(),
        [read_start, read_end](const MappedSegmentRange &range) {
            const auto range_start = static_cast<std::uint64_t>(range.guest_start);
            const auto range_end = range_start + range.memory_size;
            return read_start >= range_start && read_end <= range_end &&
                (range.guest_flags & supported_guest_flags) != 0;
        });
    if (mapping == mapped_segment_ranges_.end()) {
        error = "The requested guest range is not inside a readable mapped segment.";
        return false;
    }

    std::memcpy(output.data(), static_cast<const std::uint8_t *>(base_) + read_start, output.size());
    return true;
}

bool GuestMemory::unmap_segment(std::uint32_t guest_address,
    std::uint32_t memory_size,
    std::string &error) {
    if (mapped_segment_ranges_.size() != 1 ||
        mapped_segment_ranges_.front().guest_start != guest_address ||
        mapped_segment_ranges_.front().memory_size != memory_size) {
        error = "Individual unmapping is only supported for a single mapped segment.";
        return false;
    }
    return unmap_all_segments(error);
}

bool GuestMemory::unmap_all_segments(std::string &error) {
    bool success = true;
    for (const auto &range : mapped_page_ranges_) {
        auto *pointer = static_cast<std::uint8_t *>(base_) + range.page_start;
#ifdef _WIN32
        if (!VirtualFree(pointer, static_cast<std::size_t>(range.page_size), MEM_DECOMMIT)) {
            if (success) {
                error = "Could not decommit guest segment pages: " + host_error_message();
            }
            success = false;
        }
#else
        if (mprotect(pointer, static_cast<std::size_t>(range.page_size), PROT_NONE) != 0) {
            if (success) {
                error = "Could not protect unmapped guest segment pages: " + host_error_message();
            }
            success = false;
        } else {
            (void)madvise(pointer, static_cast<std::size_t>(range.page_size), MADV_DONTNEED);
        }
#endif
    }
    mapped_page_ranges_.clear();
    mapped_segment_ranges_.clear();
    return success;
}

void GuestMemory::release() {
    if (base_ != nullptr) {
#ifdef _WIN32
        (void)VirtualFree(base_, 0, MEM_RELEASE);
#else
        (void)munmap(base_, static_cast<std::size_t>(size_));
#endif
    }
    base_ = nullptr;
    size_ = 0;
    host_page_size_ = 0;
    mapped_page_ranges_.clear();
    mapped_segment_ranges_.clear();
}

} // namespace vita3k::ios
