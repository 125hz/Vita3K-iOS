#include <vita3k_ios/GuestHeap.h>

#include <vita3k_ios/GuestMemory.h>

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace vita3k::ios {
namespace {

constexpr std::uint32_t guest_read_write_flags = 3;
constexpr std::uint32_t default_alignment = 16;
constexpr std::uint32_t allocation_granularity = 8;

bool valid_alignment(std::uint32_t alignment) {
    return alignment >= 4 && (alignment & (alignment - 1u)) == 0;
}

} // namespace

bool GuestHeap::initialize(GuestMemory &memory,
    std::uint32_t base,
    std::uint32_t size,
    std::string &error) {
    if (ready()) {
        error = "The guest heap is already initialized.";
        return false;
    }
    if (base == 0 || size == 0 || !memory.map_dynamic_segment(
            base, size, guest_read_write_flags, error)) {
        if (error.empty()) {
            error = "The guest heap range is invalid.";
        }
        return false;
    }
    memory_ = &memory;
    base_ = base;
    size_ = size;
    blocks_.push_back({
        .address = base,
        .size = size,
        .allocated = false
    });
    return true;
}

std::uint32_t GuestHeap::allocate(std::uint32_t size,
    std::uint32_t alignment,
    bool zero,
    std::string &error) {
    if (!ready()) {
        error = "The guest heap is not initialized.";
        return 0;
    }
    if (alignment == 0) {
        alignment = default_alignment;
    }
    if (!valid_alignment(alignment)) {
        error = "The allocation alignment is not a power of two of at least four bytes.";
        return 0;
    }
    const auto effective_size = std::max(size, 1u);
    const auto rounded_size_64 = (static_cast<std::uint64_t>(effective_size)
        + allocation_granularity - 1u) & ~(static_cast<std::uint64_t>(allocation_granularity) - 1u);
    if (rounded_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The allocation size overflows the guest address space.";
        return 0;
    }
    const auto rounded_size = static_cast<std::uint32_t>(rounded_size_64);

    for (std::size_t index = 0; index < blocks_.size(); ++index) {
        const auto block = blocks_[index];
        if (block.allocated) {
            continue;
        }
        const auto aligned_address_64 = (static_cast<std::uint64_t>(block.address)
            + alignment - 1u) & ~(static_cast<std::uint64_t>(alignment) - 1u);
        const auto allocation_end = aligned_address_64 + rounded_size;
        const auto block_end = static_cast<std::uint64_t>(block.address) + block.size;
        if (aligned_address_64 > std::numeric_limits<std::uint32_t>::max()
            || allocation_end > block_end) {
            continue;
        }
        const auto aligned_address = static_cast<std::uint32_t>(aligned_address_64);
        std::vector<Block> replacement;
        if (aligned_address > block.address) {
            replacement.push_back({
                .address = block.address,
                .size = aligned_address - block.address,
                .allocated = false
            });
        }
        replacement.push_back({
            .address = aligned_address,
            .size = rounded_size,
            .allocated = true
        });
        if (allocation_end < block_end) {
            replacement.push_back({
                .address = static_cast<std::uint32_t>(allocation_end),
                .size = static_cast<std::uint32_t>(block_end - allocation_end),
                .allocated = false
            });
        }
        if (zero) {
            std::vector<std::uint8_t> zeros(rounded_size, 0);
            if (!memory_->write(aligned_address, zeros, error)) {
                return 0;
            }
        }
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
        blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(index),
            replacement.begin(), replacement.end());

        ++stats_.allocation_count;
        stats_.live_bytes += rounded_size;
        stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.live_bytes);
        return aligned_address;
    }
    error = "The bounded guest heap is out of memory.";
    return 0;
}

std::uint32_t GuestHeap::reallocate(std::uint32_t address,
    std::uint32_t size,
    std::string &error) {
    ++stats_.realloc_count;
    if (address == 0) {
        return allocate(size, default_alignment, false, error);
    }
    if (size == 0) {
        if (!free(address, error)) {
            return 0;
        }
        return 0;
    }
    const auto found = std::ranges::find(blocks_, address, &Block::address);
    if (found == blocks_.end() || !found->allocated) {
        error = "The realloc pointer does not identify a live heap allocation.";
        return 0;
    }
    const auto old_size = found->size;
    if (size <= old_size) {
        const auto rounded_size = static_cast<std::uint32_t>((static_cast<std::uint64_t>(size)
            + allocation_granularity - 1u) & ~(static_cast<std::uint64_t>(allocation_granularity) - 1u));
        if (rounded_size < old_size) {
            const auto index = static_cast<std::size_t>(std::distance(blocks_.begin(), found));
            blocks_[index].size = rounded_size;
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(index + 1), {
                .address = address + rounded_size,
                .size = old_size - rounded_size,
                .allocated = false
            });
            stats_.live_bytes -= old_size - rounded_size;
            coalesce();
        }
        return address;
    }

    std::vector<std::uint8_t> old_data(old_size);
    if (!memory_->read(address, old_data, error)) {
        return 0;
    }
    const auto new_address = allocate(size, default_alignment, false, error);
    if (new_address == 0) {
        return 0;
    }
    if (!memory_->write(new_address, old_data, error)) {
        std::string ignored;
        (void)free(new_address, ignored);
        return 0;
    }
    if (!free(address, error)) {
        return 0;
    }
    return new_address;
}

bool GuestHeap::free(std::uint32_t address, std::string &error) {
    if (address == 0) {
        return true;
    }
    const auto found = std::ranges::find(blocks_, address, &Block::address);
    if (found == blocks_.end() || !found->allocated) {
        error = "The free pointer does not identify a live heap allocation.";
        return false;
    }
    found->allocated = false;
    stats_.live_bytes -= found->size;
    ++stats_.free_count;
    coalesce();
    return true;
}

bool GuestHeap::usable_size(std::uint32_t address, std::uint32_t &size, std::string &error) const {
    if (address == 0) {
        size = 0;
        return true;
    }
    const auto found = std::ranges::find(blocks_, address, &Block::address);
    if (found == blocks_.end() || !found->allocated) {
        error = "The usable-size pointer does not identify a live heap allocation.";
        return false;
    }
    size = found->size;
    return true;
}

void GuestHeap::coalesce() {
    for (std::size_t index = 1; index < blocks_.size();) {
        auto &previous = blocks_[index - 1];
        const auto &current = blocks_[index];
        if (!previous.allocated && !current.allocated
            && static_cast<std::uint64_t>(previous.address) + previous.size == current.address) {
            previous.size += current.size;
            blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

} // namespace vita3k::ios
