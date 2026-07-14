#pragma once

#include <cstdint>
#include <string>

namespace vita3k::ios {

enum class HostTouchPhase : std::uint8_t {
    began,
    moved,
    ended,
    cancelled
};

inline constexpr std::uint32_t host_button_a = 1u << 0;
inline constexpr std::uint32_t host_button_b = 1u << 1;
inline constexpr std::uint32_t host_button_x = 1u << 2;
inline constexpr std::uint32_t host_button_y = 1u << 3;
inline constexpr std::uint32_t host_button_left_shoulder = 1u << 4;
inline constexpr std::uint32_t host_button_right_shoulder = 1u << 5;
inline constexpr std::uint32_t host_button_left_trigger = 1u << 6;
inline constexpr std::uint32_t host_button_right_trigger = 1u << 7;
inline constexpr std::uint32_t host_button_dpad_up = 1u << 8;
inline constexpr std::uint32_t host_button_dpad_down = 1u << 9;
inline constexpr std::uint32_t host_button_dpad_left = 1u << 10;
inline constexpr std::uint32_t host_button_dpad_right = 1u << 11;

struct HostControllerSample {
    float left_x{};
    float left_y{};
    float right_x{};
    float right_y{};
    std::uint32_t buttons{};
};

struct HostInputStatus {
    bool touch_surface_attached{};
    bool touch_active{};
    bool touch_sample_received{};
    bool controller_connected{};
    bool controller_sample_received{};
    double surface_width{};
    double surface_height{};
    std::uint64_t active_touch_identifier{};
    double last_touch_x{};
    double last_touch_y{};
    HostTouchPhase last_touch_phase{HostTouchPhase::cancelled};
    std::uint64_t touch_sample_count{};
    std::uint64_t controller_sample_count{};
    HostControllerSample controller{};
};

class HostInput {
public:
    bool attach_touch_surface(double width, double height, std::string &error);
    bool submit_touch(std::uint64_t identifier, double x, double y, HostTouchPhase phase,
        std::string &error);
    void set_controller_connected(bool connected);
    bool submit_controller(HostControllerSample sample, std::string &error);
    [[nodiscard]] HostInputStatus status() const;

private:
    HostInputStatus status_{};
};

} // namespace vita3k::ios
