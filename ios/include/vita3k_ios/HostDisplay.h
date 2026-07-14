#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vita3k::ios {

struct HostDisplayFrame {
    std::uint64_t identifier;
    std::uint32_t width;
    std::uint32_t height;
    double red;
    double green;
    double blue;
    double alpha;
};

struct HostDisplayStatus {
    bool attached;
    bool frame_in_flight;
    bool first_frame_presented;
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t submitted_frame_count;
    std::uint64_t presented_frame_count;
    std::string detail;
};

class HostDisplay {
public:
    bool attach(std::uint32_t width, std::uint32_t height, std::string &error);
    std::optional<HostDisplayFrame> acquire_frame(std::uint32_t width, std::uint32_t height,
        std::string &error);
    bool complete_frame(std::uint64_t identifier, bool presented, std::string detail,
        std::string &error);
    [[nodiscard]] HostDisplayStatus status() const;

private:
    HostDisplayStatus status_{};
    std::uint64_t next_frame_identifier_{1};
    std::uint64_t in_flight_identifier_{};
};

} // namespace vita3k::ios
