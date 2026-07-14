#include <vita3k_ios/HostDisplay.h>

#include <utility>

namespace vita3k::ios {

bool HostDisplay::attach(std::uint32_t width, std::uint32_t height, std::string &error) {
    if (width == 0 || height == 0) {
        error = "The Metal drawable must have a non-zero extent.";
        return false;
    }
    status_.attached = true;
    status_.width = width;
    status_.height = height;
    status_.detail = "Metal host attached; waiting for the first core-owned frame.";
    error.clear();
    return true;
}

std::optional<HostDisplayFrame> HostDisplay::acquire_frame(std::uint32_t width,
    std::uint32_t height, std::string &error) {
    if (!status_.attached && !attach(width, height, error)) {
        return std::nullopt;
    }
    if (width == 0 || height == 0) {
        error = "The Metal drawable must have a non-zero extent.";
        return std::nullopt;
    }
    status_.width = width;
    status_.height = height;
    if (status_.frame_in_flight || status_.first_frame_presented) {
        error.clear();
        return std::nullopt;
    }

    in_flight_identifier_ = next_frame_identifier_++;
    status_.frame_in_flight = true;
    ++status_.submitted_frame_count;
    status_.detail = "Core-owned diagnostic frame submitted to Metal.";
    error.clear();
    return HostDisplayFrame{
        .identifier = in_flight_identifier_,
        .width = width,
        .height = height,
        .red = 0.035,
        .green = 0.16,
        .blue = 0.36,
        .alpha = 1.0
    };
}

bool HostDisplay::complete_frame(std::uint64_t identifier, bool presented, std::string detail,
    std::string &error) {
    if (!status_.frame_in_flight || identifier != in_flight_identifier_) {
        error = "The completed Metal frame does not match the frame in flight.";
        return false;
    }

    status_.frame_in_flight = false;
    in_flight_identifier_ = 0;
    if (presented) {
        status_.first_frame_presented = true;
        ++status_.presented_frame_count;
        status_.detail = "First core-owned Metal frame presented.";
    } else {
        status_.detail = detail.empty() ? "Metal did not present the diagnostic frame."
                                        : std::move(detail);
    }
    error.clear();
    return true;
}

HostDisplayStatus HostDisplay::status() const {
    return status_;
}

} // namespace vita3k::ios
