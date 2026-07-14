#import <vita3k_ios/IOSInputAdapter.h>

#import <GameController/GameController.h>
#import <dispatch/dispatch.h>

#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/IOSLogger.h>

#include <cstdint>
#include <string>

namespace {

vita3k::ios::HostTouchPhase host_phase(VitaTouchPhase phase) {
    switch (phase) {
    case VitaTouchPhaseBegan:
        return vita3k::ios::HostTouchPhase::began;
    case VitaTouchPhaseMoved:
        return vita3k::ios::HostTouchPhase::moved;
    case VitaTouchPhaseEnded:
        return vita3k::ios::HostTouchPhase::ended;
    case VitaTouchPhaseCancelled:
        return vita3k::ios::HostTouchPhase::cancelled;
    }
    return vita3k::ios::HostTouchPhase::cancelled;
}

} // namespace

@interface VitaInputAdapter ()
@property(nonatomic, weak) UIView *view;
@property(nonatomic, strong) GCController *activeController;
@property(nonatomic, copy) VitaInputCompletion completion;
@property(nonatomic) CGSize lastTouchSurfaceSize;
@end

@implementation VitaInputAdapter

- (instancetype)initWithView:(UIView *)view completion:(VitaInputCompletion)completion {
    self = [super init];
    if (self) {
        _view = view;
        _completion = [completion copy];
        _lastTouchSurfaceSize = CGSizeZero;
        NSNotificationCenter *notifications = NSNotificationCenter.defaultCenter;
        [notifications addObserver:self selector:@selector(controllerDidConnect:)
                              name:GCControllerDidConnectNotification object:nil];
        [notifications addObserver:self selector:@selector(controllerDidDisconnect:)
                              name:GCControllerDidDisconnectNotification object:nil];
        GCController *controller = GCController.controllers.firstObject;
        if (controller != nil) {
            [self configureController:controller];
        }
    }
    return self;
}

- (void)dealloc {
    self.activeController.extendedGamepad.valueChangedHandler = nil;
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)notifyCompletion {
    VitaInputCompletion completion = self.completion;
    if (completion == nil) {
        return;
    }
    if (NSThread.isMainThread) {
        completion();
    } else {
        dispatch_async(dispatch_get_main_queue(), completion);
    }
}

- (void)attachTouchSurface {
    const CGSize size = self.view.bounds.size;
    if (CGSizeEqualToSize(size, self.lastTouchSurfaceSize)) {
        return;
    }
    std::string error;
    if (!vita3k::ios::attach_host_input_surface(size.width, size.height, error)) {
        vita3k::ios::log_message("ERROR", error);
        return;
    }
    self.lastTouchSurfaceSize = size;
    [self notifyCompletion];
}

- (void)submitTouches:(NSSet<UITouch *> *)touches phase:(VitaTouchPhase)phase {
    for (UITouch *touch in touches) {
        const CGPoint point = [touch locationInView:self.view];
        const auto identifier = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>((__bridge const void *)touch));
        std::string error;
        if (!vita3k::ios::submit_host_touch(identifier, point.x, point.y,
                host_phase(phase), error)) {
            vita3k::ios::log_message("ERROR", error);
        }
    }
    [self notifyCompletion];
}

- (void)controllerDidConnect:(NSNotification *)notification {
    GCController *controller = (GCController *)notification.object;
    if (controller != nil) {
        [self configureController:controller];
    }
}

- (void)controllerDidDisconnect:(NSNotification *)notification {
    if (notification.object != self.activeController) {
        return;
    }
    self.activeController.extendedGamepad.valueChangedHandler = nil;
    self.activeController = nil;
    vita3k::ios::set_host_controller_connected(false);
    [self notifyCompletion];
}

- (void)configureController:(GCController *)controller {
    self.activeController.extendedGamepad.valueChangedHandler = nil;
    self.activeController = controller;
    vita3k::ios::set_host_controller_connected(true);

    GCExtendedGamepad *gamepad = controller.extendedGamepad;
    if (gamepad == nil) {
        [self notifyCompletion];
        return;
    }

    __weak VitaInputAdapter *weakSelf = self;
    gamepad.valueChangedHandler = ^(GCExtendedGamepad *changedGamepad,
                                    GCControllerElement *element) {
        (void)element;
        [weakSelf publishGamepad:changedGamepad];
    };
    [self publishGamepad:gamepad];
}

- (void)publishGamepad:(GCExtendedGamepad *)gamepad {
    std::uint32_t buttons = 0;
    buttons |= gamepad.buttonA.isPressed ? vita3k::ios::host_button_a : 0;
    buttons |= gamepad.buttonB.isPressed ? vita3k::ios::host_button_b : 0;
    buttons |= gamepad.buttonX.isPressed ? vita3k::ios::host_button_x : 0;
    buttons |= gamepad.buttonY.isPressed ? vita3k::ios::host_button_y : 0;
    buttons |= gamepad.leftShoulder.isPressed ? vita3k::ios::host_button_left_shoulder : 0;
    buttons |= gamepad.rightShoulder.isPressed ? vita3k::ios::host_button_right_shoulder : 0;
    buttons |= gamepad.leftTrigger.isPressed ? vita3k::ios::host_button_left_trigger : 0;
    buttons |= gamepad.rightTrigger.isPressed ? vita3k::ios::host_button_right_trigger : 0;
    buttons |= gamepad.dpad.up.isPressed ? vita3k::ios::host_button_dpad_up : 0;
    buttons |= gamepad.dpad.down.isPressed ? vita3k::ios::host_button_dpad_down : 0;
    buttons |= gamepad.dpad.left.isPressed ? vita3k::ios::host_button_dpad_left : 0;
    buttons |= gamepad.dpad.right.isPressed ? vita3k::ios::host_button_dpad_right : 0;

    const vita3k::ios::HostControllerSample sample{
        .left_x = gamepad.leftThumbstick.xAxis.value,
        .left_y = gamepad.leftThumbstick.yAxis.value,
        .right_x = gamepad.rightThumbstick.xAxis.value,
        .right_y = gamepad.rightThumbstick.yAxis.value,
        .buttons = buttons
    };
    std::string error;
    if (!vita3k::ios::submit_host_controller(sample, error)) {
        vita3k::ios::log_message("ERROR", error);
    }
    [self notifyCompletion];
}

@end
