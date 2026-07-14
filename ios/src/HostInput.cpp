#include <vita3k_ios/HostInput.h>

#include <algorithm>
#include <cmath>

namespace vita3k::ios {

bool HostInput::attach_touch_surface(double width, double height, std::string &error) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0 || height < 1.0) {
        error = "The UIKit touch surface must have a finite non-zero extent.";
        return false;
    }
    status_.touch_surface_attached = true;
    status_.surface_width = width;
    status_.surface_height = height;
    error.clear();
    return true;
}

bool HostInput::submit_touch(std::uint64_t identifier, double x, double y,
    HostTouchPhase phase, std::string &error) {
    if (!status_.touch_surface_attached) {
        error = "The UIKit touch surface is not attached.";
        return false;
    }
    if (identifier == 0 || !std::isfinite(x) || !std::isfinite(y)) {
        error = "The UIKit touch sample is invalid.";
        return false;
    }

    status_.active_touch_identifier = identifier;
    status_.last_touch_x = std::clamp(x / status_.surface_width, 0.0, 1.0);
    status_.last_touch_y = std::clamp(y / status_.surface_height, 0.0, 1.0);
    status_.last_touch_phase = phase;
    status_.touch_active = phase == HostTouchPhase::began || phase == HostTouchPhase::moved;
    if (!status_.touch_active) {
        status_.active_touch_identifier = 0;
    }
    status_.touch_sample_received = true;
    ++status_.touch_sample_count;
    error.clear();
    return true;
}

void HostInput::set_controller_connected(bool connected) {
    status_.controller_connected = connected;
    if (!connected) {
        status_.controller = {};
    }
}

bool HostInput::submit_controller(HostControllerSample sample, std::string &error) {
    if (!status_.controller_connected) {
        error = "A controller sample arrived without a connected controller.";
        return false;
    }
    const bool axes_valid = std::isfinite(sample.left_x) && std::isfinite(sample.left_y) &&
        std::isfinite(sample.right_x) && std::isfinite(sample.right_y);
    if (!axes_valid) {
        error = "The controller sample contains a non-finite axis value.";
        return false;
    }
    sample.left_x = std::clamp(sample.left_x, -1.0f, 1.0f);
    sample.left_y = std::clamp(sample.left_y, -1.0f, 1.0f);
    sample.right_x = std::clamp(sample.right_x, -1.0f, 1.0f);
    sample.right_y = std::clamp(sample.right_y, -1.0f, 1.0f);
    status_.controller = sample;
    status_.controller_sample_received = true;
    ++status_.controller_sample_count;
    error.clear();
    return true;
}

HostInputStatus HostInput::status() const {
    return status_;
}

} // namespace vita3k::ios
