#include <vita3k_ios/GuestMemory.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>

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
    if (!ready() || host_page_size_ == 0) {
        error = "Guest memory is not reserved.";
        return false;
    }
    if (memory_size == 0 || file_data.size() > memory_size) {
        error = "The guest segment has invalid file and memory sizes.";
        return false;
    }
    if ((guest_flags & ~(guest_flag_read | guest_flag_write | guest_flag_execute)) != 0) {
        error = "The guest segment contains unsupported permission flags.";
        return false;
    }

    const auto segment_start = static_cast<std::uint64_t>(guest_address);
    const auto segment_end = segment_start + memory_size;
    if (segment_end > size_) {
        error = "The guest segment exceeds the reserved address space.";
        return false;
    }

    const auto page_size = static_cast<std::uint64_t>(host_page_size_);
    const auto page_start = (segment_start / page_size) * page_size;
    const auto page_end = ((segment_end + page_size - 1) / page_size) * page_size;
    if (page_end > size_ || page_end <= page_start) {
        error = "The aligned guest segment range is invalid.";
        return false;
    }
    const auto mapped_size = page_end - page_start;
    const bool overlaps = std::any_of(mapped_ranges_.begin(), mapped_ranges_.end(),
        [page_start, page_end](const MappedRange &range) {
            return page_start < range.page_start + range.page_size &&
                range.page_start < page_end;
        });
    if (overlaps) {
        error = "The guest segment overlaps an already mapped host page.";
        return false;
    }

    auto *page_pointer = static_cast<std::uint8_t *>(base_) + page_start;
#ifdef _WIN32
    void *committed = VirtualAlloc(page_pointer, static_cast<std::size_t>(mapped_size),
        MEM_COMMIT, PAGE_READWRITE);
    if (committed != page_pointer) {
        error = "Could not commit guest segment pages: " + host_error_message();
        return false;
    }
#else
    if (mprotect(page_pointer, static_cast<std::size_t>(mapped_size), PROT_READ | PROT_WRITE) != 0) {
        error = "Could not make guest segment pages writable: " + host_error_message();
        return false;
    }
#endif

    auto *segment_pointer = static_cast<std::uint8_t *>(base_) + segment_start;
    if (!file_data.empty()) {
        std::memcpy(segment_pointer, file_data.data(), file_data.size());
    }
    std::memset(segment_pointer + file_data.size(), 0, memory_size - file_data.size());

#ifdef _WIN32
    const DWORD final_protection = (guest_flags & guest_flag_write) != 0
        ? PAGE_READWRITE
        : ((guest_flags & (guest_flag_read | guest_flag_execute)) != 0 ? PAGE_READONLY : PAGE_NOACCESS);
    DWORD previous_protection = 0;
    if (!VirtualProtect(page_pointer, static_cast<std::size_t>(mapped_size),
            final_protection, &previous_protection)) {
        error = "Could not apply final guest segment protection: " + host_error_message();
        (void)VirtualFree(page_pointer, static_cast<std::size_t>(mapped_size), MEM_DECOMMIT);
        return false;
    }
#else
    int final_protection = PROT_NONE;
    if ((guest_flags & guest_flag_write) != 0) {
        final_protection = PROT_READ | PROT_WRITE;
    } else if ((guest_flags & (guest_flag_read | guest_flag_execute)) != 0) {
        // Guest execute permission does not require host execute permission. The
        // emulator reads these bytes and translates them into a separate JIT area.
        final_protection = PROT_READ;
    }
    if (mprotect(page_pointer, static_cast<std::size_t>(mapped_size), final_protection) != 0) {
        error = "Could not apply final guest segment protection: " + host_error_message();
        (void)mprotect(page_pointer, static_cast<std::size_t>(mapped_size), PROT_NONE);
        (void)madvise(page_pointer, static_cast<std::size_t>(mapped_size), MADV_DONTNEED);
        return false;
    }
#endif

    mapped_ranges_.push_back({
        .page_start = page_start,
        .page_size = mapped_size,
        .guest_start = guest_address,
        .memory_size = memory_size,
        .guest_flags = guest_flags
    });
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
    const auto mapping = std::find_if(mapped_ranges_.begin(), mapped_ranges_.end(),
        [read_start, read_end](const MappedRange &range) {
            const auto range_start = static_cast<std::uint64_t>(range.guest_start);
            const auto range_end = range_start + range.memory_size;
            return read_start >= range_start && read_end <= range_end &&
                (range.guest_flags & (guest_flag_read | guest_flag_execute)) != 0;
        });
    if (mapping == mapped_ranges_.end()) {
        error = "The requested guest range is not inside a readable mapped segment.";
        return false;
    }

    std::memcpy(output.data(), static_cast<const std::uint8_t *>(base_) + read_start, output.size());
    return true;
}

bool GuestMemory::unmap_segment(std::uint32_t guest_address,
    std::uint32_t memory_size,
    std::string &error) {
    const auto mapping = std::find_if(mapped_ranges_.begin(), mapped_ranges_.end(),
        [guest_address, memory_size](const MappedRange &range) {
            return range.guest_start == guest_address && range.memory_size == memory_size;
        });
    if (mapping == mapped_ranges_.end()) {
        error = "The requested guest segment is not mapped.";
        return false;
    }

    auto *page_pointer = static_cast<std::uint8_t *>(base_) + mapping->page_start;
#ifdef _WIN32
    if (!VirtualFree(page_pointer, static_cast<std::size_t>(mapping->page_size), MEM_DECOMMIT)) {
        error = "Could not decommit guest segment pages: " + host_error_message();
        return false;
    }
#else
    if (mprotect(page_pointer, static_cast<std::size_t>(mapping->page_size), PROT_NONE) != 0) {
        error = "Could not protect unmapped guest segment pages: " + host_error_message();
        return false;
    }
    (void)madvise(page_pointer, static_cast<std::size_t>(mapping->page_size), MADV_DONTNEED);
#endif
    mapped_ranges_.erase(mapping);
    return true;
}

void GuestMemory::release() {
    if (base_ == nullptr) {
        return;
    }
#ifdef _WIN32
    (void)VirtualFree(base_, 0, MEM_RELEASE);
#else
    (void)munmap(base_, static_cast<std::size_t>(size_));
#endif
    base_ = nullptr;
    size_ = 0;
    host_page_size_ = 0;
    mapped_ranges_.clear();
}

} // namespace vita3k::ios
