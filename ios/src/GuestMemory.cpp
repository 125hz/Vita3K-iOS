#include <vita3k_ios/GuestMemory.h>

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
}

} // namespace vita3k::ios
