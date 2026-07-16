// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <vita3k_ios/VirtualController.h>

#include <SDL3/SDL.h>

#import <UIKit/UIKit.h>

static SDL_JoystickID g_virtual_joystick_id = 0;
static SDL_Joystick *g_virtual_joystick = nullptr;

@interface Vita3KVirtualControllerView : UIView
@property(nonatomic, strong) NSMutableArray<UIButton *> *controllerButtons;
@end

@implementation Vita3KVirtualControllerView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;

    self.backgroundColor = UIColor.clearColor;
    self.multipleTouchEnabled = YES;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.controllerButtons = [NSMutableArray array];

    [self addButton:@"UP" gamepadButton:SDL_GAMEPAD_BUTTON_DPAD_UP accessibilityLabel:@"D-pad up"];
    [self addButton:@"DOWN" gamepadButton:SDL_GAMEPAD_BUTTON_DPAD_DOWN accessibilityLabel:@"D-pad down"];
    [self addButton:@"LEFT" gamepadButton:SDL_GAMEPAD_BUTTON_DPAD_LEFT accessibilityLabel:@"D-pad left"];
    [self addButton:@"RIGHT" gamepadButton:SDL_GAMEPAD_BUTTON_DPAD_RIGHT accessibilityLabel:@"D-pad right"];

    [self addButton:@"TRI" gamepadButton:SDL_GAMEPAD_BUTTON_NORTH accessibilityLabel:@"Triangle"];
    [self addButton:@"O" gamepadButton:SDL_GAMEPAD_BUTTON_EAST accessibilityLabel:@"Circle"];
    [self addButton:@"X" gamepadButton:SDL_GAMEPAD_BUTTON_SOUTH accessibilityLabel:@"Cross"];
    [self addButton:@"SQ" gamepadButton:SDL_GAMEPAD_BUTTON_WEST accessibilityLabel:@"Square"];

    [self addButton:@"L" gamepadButton:SDL_GAMEPAD_BUTTON_LEFT_SHOULDER accessibilityLabel:@"Left shoulder"];
    [self addButton:@"R" gamepadButton:SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER accessibilityLabel:@"Right shoulder"];
    [self addButton:@"SELECT" gamepadButton:SDL_GAMEPAD_BUTTON_BACK accessibilityLabel:@"Select"];
    [self addButton:@"START" gamepadButton:SDL_GAMEPAD_BUTTON_START accessibilityLabel:@"Start"];
    return self;
}

- (void)addButton:(NSString *)title
    gamepadButton:(SDL_GamepadButton)gamepadButton
accessibilityLabel:(NSString *)accessibilityLabel {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.tag = static_cast<NSInteger>(gamepadButton);
    button.multipleTouchEnabled = YES;
    button.exclusiveTouch = NO;
    button.accessibilityLabel = accessibilityLabel;
    button.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.48];
    button.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.72].CGColor;
    button.layer.borderWidth = 1.5;
    button.layer.cornerRadius = 14.0;
    button.titleLabel.font = [UIFont boldSystemFontOfSize:13.0];
    button.titleLabel.adjustsFontSizeToFitWidth = YES;
    button.contentEdgeInsets = UIEdgeInsetsMake(4, 4, 4, 4);
    [button setTitle:title forState:UIControlStateNormal];
    [button setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [button setTitleColor:[UIColor colorWithRed:0.58 green:0.85 blue:1.0 alpha:1.0]
        forState:UIControlStateHighlighted];

    [button addTarget:self action:@selector(buttonPressed:)
        forControlEvents:UIControlEventTouchDown | UIControlEventTouchDragEnter];
    [button addTarget:self action:@selector(buttonReleased:)
        forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside
                         | UIControlEventTouchCancel | UIControlEventTouchDragExit];
    [self addSubview:button];
    [self.controllerButtons addObject:button];
}

- (void)buttonPressed:(UIButton *)sender {
    if (g_virtual_joystick)
        SDL_SetJoystickVirtualButton(g_virtual_joystick,
            static_cast<int>(sender.tag), true);
}

- (void)buttonReleased:(UIButton *)sender {
    if (g_virtual_joystick)
        SDL_SetJoystickVirtualButton(g_virtual_joystick,
            static_cast<int>(sender.tag), false);
}

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    // The full-screen overlay is only interactive over a visible controller
    // button. All other touches continue to SDL and the guest touchscreen.
    for (UIButton *button in self.controllerButtons) {
        CGPoint localPoint = [button convertPoint:point fromView:self];
        if ([button pointInside:localPoint withEvent:event])
            return YES;
    }
    return NO;
}

- (void)layoutSubviews {
    [super layoutSubviews];

    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat width = CGRectGetWidth(self.bounds);
    const CGFloat height = CGRectGetHeight(self.bounds);
    const CGFloat size = MIN(66.0, MAX(48.0, height * 0.105));
    const CGFloat step = size * 0.88;
    const CGFloat left = safe.left + 18.0;
    const CGFloat right = safe.right + 18.0;
    const CGFloat bottom = safe.bottom + 20.0;
    const CGPoint dpadCenter = CGPointMake(left + size * 1.42, height - bottom - size * 1.42);
    const CGPoint faceCenter = CGPointMake(width - right - size * 1.42, height - bottom - size * 1.42);

    [self placeButton:SDL_GAMEPAD_BUTTON_DPAD_UP center:CGPointMake(dpadCenter.x, dpadCenter.y - step) size:size];
    [self placeButton:SDL_GAMEPAD_BUTTON_DPAD_DOWN center:CGPointMake(dpadCenter.x, dpadCenter.y + step) size:size];
    [self placeButton:SDL_GAMEPAD_BUTTON_DPAD_LEFT center:CGPointMake(dpadCenter.x - step, dpadCenter.y) size:size];
    [self placeButton:SDL_GAMEPAD_BUTTON_DPAD_RIGHT center:CGPointMake(dpadCenter.x + step, dpadCenter.y) size:size];

    [self placeButton:SDL_GAMEPAD_BUTTON_NORTH center:CGPointMake(faceCenter.x, faceCenter.y - step) size:size];
    [self placeButton:SDL_GAMEPAD_BUTTON_SOUTH center:CGPointMake(faceCenter.x, faceCenter.y + step) size:size];
    [self placeButton:SDL_GAMEPAD_BUTTON_WEST center:CGPointMake(faceCenter.x - step, faceCenter.y) size:size];
    [self placeButton:SDL_GAMEPAD_BUTTON_EAST center:CGPointMake(faceCenter.x + step, faceCenter.y) size:size];

    const CGFloat shoulderWidth = MAX(82.0, size * 1.45);
    const CGFloat shoulderHeight = MAX(38.0, size * 0.62);
    const CGFloat shoulderY = safe.top + 12.0 + shoulderHeight / 2.0;
    [self placeButton:SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
        center:CGPointMake(left + shoulderWidth / 2.0, shoulderY)
        width:shoulderWidth height:shoulderHeight];
    [self placeButton:SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
        center:CGPointMake(width - right - shoulderWidth / 2.0, shoulderY)
        width:shoulderWidth height:shoulderHeight];

    const CGFloat systemWidth = MAX(70.0, size * 1.18);
    const CGFloat systemHeight = MAX(32.0, size * 0.52);
    const CGFloat systemY = height - bottom - systemHeight / 2.0;
    [self placeButton:SDL_GAMEPAD_BUTTON_BACK
        center:CGPointMake(width / 2.0 - systemWidth * 0.62, systemY)
        width:systemWidth height:systemHeight];
    [self placeButton:SDL_GAMEPAD_BUTTON_START
        center:CGPointMake(width / 2.0 + systemWidth * 0.62, systemY)
        width:systemWidth height:systemHeight];
}

- (UIButton *)buttonForGamepadButton:(SDL_GamepadButton)gamepadButton {
    for (UIButton *button in self.controllerButtons) {
        if (button.tag == static_cast<NSInteger>(gamepadButton))
            return button;
    }
    return nil;
}

- (void)placeButton:(SDL_GamepadButton)gamepadButton center:(CGPoint)center size:(CGFloat)size {
    [self placeButton:gamepadButton center:center width:size height:size];
}

- (void)placeButton:(SDL_GamepadButton)gamepadButton
             center:(CGPoint)center
              width:(CGFloat)width
             height:(CGFloat)height {
    UIButton *button = [self buttonForGamepadButton:gamepadButton];
    button.bounds = CGRectMake(0, 0, width, height);
    button.center = center;
    button.layer.cornerRadius = MIN(width, height) * 0.28;
}

- (void)releaseAllButtons {
    if (!g_virtual_joystick)
        return;
    for (UIButton *button in self.controllerButtons)
        SDL_SetJoystickVirtualButton(g_virtual_joystick,
            static_cast<int>(button.tag), false);
}

@end

static Vita3KVirtualControllerView *g_overlay = nil;

static UIWindow *activeWindow() {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]
            || scene.activationState == UISceneActivationStateUnattached)
            continue;
        UIWindowScene *windowScene = (UIWindowScene *)scene;
        for (UIWindow *window in windowScene.windows) {
            if (window.isKeyWindow)
                return window;
        }
        for (UIWindow *window in windowScene.windows) {
            if (!window.hidden)
                return window;
        }
    }
    return nil;
}

static void performOnMainThread(dispatch_block_t block) {
    if (NSThread.isMainThread)
        block();
    else
        dispatch_async(dispatch_get_main_queue(), block);
}

bool vita3k_ios_attach_virtual_controller() {
    vita3k_ios_detach_virtual_controller();
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        SDL_Log("Vita3K iOS: cannot attach touch controller before SDL gamepad init");
        return false;
    }

    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Vita3K iOS Touch Controller";

    g_virtual_joystick_id = SDL_AttachVirtualJoystick(&desc);
    if (g_virtual_joystick_id == 0) {
        SDL_Log("Vita3K iOS: virtual controller attach failed: %s", SDL_GetError());
        return false;
    }

    g_virtual_joystick = SDL_OpenJoystick(g_virtual_joystick_id);
    if (!g_virtual_joystick) {
        SDL_Log("Vita3K iOS: virtual controller open failed: %s", SDL_GetError());
        SDL_DetachVirtualJoystick(g_virtual_joystick_id);
        g_virtual_joystick_id = 0;
        return false;
    }

    SDL_Log("Vita3K iOS: virtual touch controller attached (joystick=%u)",
        static_cast<unsigned>(g_virtual_joystick_id));
    return true;
}

void vita3k_ios_show_virtual_controller() {
    performOnMainThread(^{
        if (g_overlay)
            return;
        UIWindow *window = activeWindow();
        if (!window) {
            SDL_Log("Vita3K iOS: no active window for touch controller overlay");
            return;
        }
        g_overlay = [[Vita3KVirtualControllerView alloc] initWithFrame:window.bounds];
        [window addSubview:g_overlay];
        [window bringSubviewToFront:g_overlay];
        SDL_Log("Vita3K iOS: virtual touch controller visible");
    });
}

void vita3k_ios_hide_virtual_controller() {
    performOnMainThread(^{
        [g_overlay releaseAllButtons];
        [g_overlay removeFromSuperview];
        g_overlay = nil;
    });
}

void vita3k_ios_detach_virtual_controller() {
    if (g_virtual_joystick)
        SDL_CloseJoystick(g_virtual_joystick);
    if (g_virtual_joystick_id != 0)
        SDL_DetachVirtualJoystick(g_virtual_joystick_id);
    g_virtual_joystick = nullptr;
    g_virtual_joystick_id = 0;
}
