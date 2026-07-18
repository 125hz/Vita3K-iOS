// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#include <vita3k_ios/VirtualController.h>
#include <vita3k_ios/NativeFrontend.h>

#include <SDL3/SDL.h>

#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <cmath>

static SDL_JoystickID g_virtual_joystick_id = 0;
static SDL_Joystick *g_virtual_joystick = nullptr;
static std::atomic_bool g_physical_controller_connected = false;
static std::atomic<float> g_safe_area_top_pixels = 0.0f;
static NSMutableDictionary *g_controls_config = nil;

@class Vita3KVirtualControllerView;
static Vita3KVirtualControllerView *g_overlay = nil;
static UIView *g_options_view = nil;
static UIView *g_game_menu = nil;

static UIWindow *activeWindow() {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]
            || scene.activationState == UISceneActivationStateUnattached)
            continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            if (window.isKeyWindow)
                return window;
        }
    }
    return nil;
}

static UIVisualEffect *glassEffect(const BOOL interactive = YES) {
    if (@available(iOS 26.0, *)) {
        UIGlassEffect *effect = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
        effect.interactive = interactive;
        effect.tintColor = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
            return traits.userInterfaceStyle == UIUserInterfaceStyleDark
                ? [UIColor colorWithWhite:0.04 alpha:0.12]
                : [UIColor colorWithWhite:1.0 alpha:0.10];
        }];
        return effect;
    }
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterial];
}

static constexpr NSInteger glassBackgroundTag = 0x3301;

static void installGlassBackground(UIView *view, const BOOL interactive = YES) {
    UIVisualEffectView *glass = [[UIVisualEffectView alloc] initWithEffect:glassEffect(interactive)];
    glass.tag = glassBackgroundTag;
    glass.userInteractionEnabled = NO;
    glass.frame = view.bounds;
    glass.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [view insertSubview:glass atIndex:0];
}

static void layoutGlassBackground(UIView *view) {
    UIView *glass = [view viewWithTag:glassBackgroundTag];
    glass.frame = view.bounds;
    glass.layer.cornerRadius = view.layer.cornerRadius;
    glass.clipsToBounds = YES;
}

static NSString *configPath() {
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [[documents stringByAppendingPathComponent:@"Tsubomi"] stringByAppendingPathComponent:@"ios_controls.json"];
}

static NSMutableDictionary *element(CGFloat x, CGFloat y, BOOL visible) {
    return [@{@"x": @(x), @"y": @(y), @"visible": @(visible)} mutableCopy];
}

static NSMutableDictionary *landscapeElements() {
    return [@{
        @"dpad_up": element(0.14, 0.66, YES), @"dpad_down": element(0.14, 0.86, YES),
        @"dpad_left": element(0.06, 0.76, YES), @"dpad_right": element(0.22, 0.76, YES),
        @"triangle": element(0.86, 0.66, YES), @"cross": element(0.86, 0.86, YES),
        @"square": element(0.78, 0.76, YES), @"circle": element(0.94, 0.76, YES),
        @"left_shoulder": element(0.09, 0.10, YES), @"right_shoulder": element(0.91, 0.10, YES),
        @"select": element(0.43, 0.91, YES), @"start": element(0.57, 0.91, YES),
        @"left_stick": element(0.29, 0.73, YES), @"right_stick": element(0.71, 0.73, YES),
        @"menu": element(0.95, 0.17, YES),
    } mutableCopy];
}

static NSMutableDictionary *portraitElements() {
    return [@{
        @"dpad_up": element(0.22, 0.59, YES), @"dpad_down": element(0.22, 0.71, YES),
        @"dpad_left": element(0.11, 0.65, YES), @"dpad_right": element(0.33, 0.65, YES),
        @"triangle": element(0.78, 0.59, YES), @"cross": element(0.78, 0.71, YES),
        @"square": element(0.67, 0.65, YES), @"circle": element(0.89, 0.65, YES),
        @"left_shoulder": element(0.15, 0.53, YES), @"right_shoulder": element(0.85, 0.53, YES),
        @"select": element(0.40, 0.94, YES), @"start": element(0.60, 0.94, YES),
        @"left_stick": element(0.20, 0.84, YES), @"right_stick": element(0.80, 0.84, YES),
        @"menu": element(0.94, 0.54, YES),
    } mutableCopy];
}

static NSMutableDictionary *defaultConfig() {
    return [@{
        @"opacity": @0.58,
        @"scale": @1.0,
        @"hideWhenPhysical": @YES,
        @"haptics": @YES,
        @"layoutVersion": @2,
        @"layouts": [@{@"landscape": landscapeElements(), @"portrait": portraitElements()} mutableCopy],
    } mutableCopy];
}

static void loadConfig();
static void saveConfig();

static NSString *orientationKey() {
    UIView *layoutView = (UIView *)g_overlay;
    UIWindow *window = activeWindow();
    const CGRect bounds = layoutView ? layoutView.bounds : window.bounds;
    return CGRectGetHeight(bounds) > CGRectGetWidth(bounds) ? @"portrait" : @"landscape";
}

static NSMutableDictionary *currentElementsConfig() {
    loadConfig();
    return g_controls_config[@"layouts"][orientationKey()];
}

static void loadConfig() {
    if (g_controls_config)
        return;
    g_controls_config = defaultConfig();
    NSData *data = [NSData dataWithContentsOfFile:configPath()];
    if (!data)
        return;
    id decoded = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
    if (![decoded isKindOfClass:NSDictionary.class])
        return;
    NSDictionary *saved = decoded;
    for (NSString *key in @[@"opacity", @"scale", @"hideWhenPhysical", @"haptics"]) {
        if (saved[key])
            g_controls_config[key] = saved[key];
    }
    NSDictionary *savedLayouts = saved[@"layouts"];
    if ([savedLayouts isKindOfClass:NSDictionary.class]) {
        for (NSString *layoutKey in @[@"landscape", @"portrait"]) {
            NSDictionary *savedElements = savedLayouts[layoutKey];
            NSMutableDictionary *elements = g_controls_config[@"layouts"][layoutKey];
            [savedElements enumerateKeysAndObjectsUsingBlock:^(id keyObject, id valueObject, __unused BOOL *stop) {
                NSString *key = [keyObject isKindOfClass:NSString.class] ? keyObject : nil;
                NSDictionary *value = [valueObject isKindOfClass:NSDictionary.class] ? valueObject : nil;
                if (value && elements[key])
                    elements[key] = [value mutableCopy];
            }];
        }
    } else if ([saved[@"elements"] isKindOfClass:NSDictionary.class]) {
        // Migrate the original single landscape layout without discarding it.
        NSMutableDictionary *elements = g_controls_config[@"layouts"][@"landscape"];
        [saved[@"elements"] enumerateKeysAndObjectsUsingBlock:^(id keyObject, id valueObject, __unused BOOL *stop) {
            NSString *key = [keyObject isKindOfClass:NSString.class] ? keyObject : nil;
            NSDictionary *value = [valueObject isKindOfClass:NSDictionary.class] ? valueObject : nil;
            if (value && elements[key])
                elements[key] = [value mutableCopy];
        }];
    }

    // Preserve user-customized positions, but move untouched v1 defaults a
    // little farther apart so opposing D-pad and face buttons do not crowd.
    if ([saved[@"layoutVersion"] integerValue] < 2) {
        void (^migrateX)(NSString *, NSString *, CGFloat, CGFloat) =
            ^(NSString *layout, NSString *key, CGFloat oldX, CGFloat newX) {
                NSMutableDictionary *entry = g_controls_config[@"layouts"][layout][key];
                if (fabs([entry[@"x"] doubleValue] - oldX) < 0.0001)
                    entry[@"x"] = @(newX);
            };
        migrateX(@"landscape", @"dpad_left", 0.08, 0.06);
        migrateX(@"landscape", @"dpad_right", 0.20, 0.22);
        migrateX(@"landscape", @"square", 0.80, 0.78);
        migrateX(@"landscape", @"circle", 0.92, 0.94);
        migrateX(@"portrait", @"dpad_left", 0.13, 0.11);
        migrateX(@"portrait", @"dpad_right", 0.31, 0.33);
        migrateX(@"portrait", @"square", 0.69, 0.67);
        migrateX(@"portrait", @"circle", 0.87, 0.89);
        g_controls_config[@"layoutVersion"] = @2;
        saveConfig();
    }
}

static void saveConfig() {
    loadConfig();
    NSString *directory = [configPath() stringByDeletingLastPathComponent];
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
    NSData *data = [NSJSONSerialization dataWithJSONObject:g_controls_config options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToFile:configPath() atomically:YES];
}

static NSMutableDictionary *elementConfig(NSString *identifier) {
    return currentElementsConfig()[identifier];
}

static void hapticTick() {
    loadConfig();
    if (![g_controls_config[@"haptics"] boolValue])
        return;
    UIImpactFeedbackGenerator *feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [feedback impactOccurred];
}

@interface Vita3KAnalogStick : UIView
@property(nonatomic) SDL_GamepadAxis horizontalAxis;
@property(nonatomic) SDL_GamepadAxis verticalAxis;
@property(nonatomic, copy) NSString *elementIdentifier;
@property(nonatomic, strong) UIView *thumb;
@property(nonatomic, strong) UIVisualEffectView *glass;
@property(nonatomic) BOOL layoutEditing;
- (void)resetAxes;
@end

@implementation Vita3KAnalogStick

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    self.layer.borderWidth = 1.5;
    self.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.55].CGColor;
    self.glass = [[UIVisualEffectView alloc] initWithEffect:glassEffect()];
    self.glass.userInteractionEnabled = NO;
    [self addSubview:self.glass];
    self.thumb = [[UIView alloc] init];
    self.thumb.backgroundColor = [UIColor colorWithWhite:0.9 alpha:0.56];
    self.thumb.layer.borderWidth = 1;
    self.thumb.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.8].CGColor;
    [self addSubview:self.thumb];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.layer.cornerRadius = CGRectGetWidth(self.bounds) / 2;
    self.glass.frame = self.bounds;
    self.glass.layer.cornerRadius = self.layer.cornerRadius;
    self.glass.clipsToBounds = YES;
    const CGFloat thumbSize = CGRectGetWidth(self.bounds) * 0.46;
    self.thumb.bounds = CGRectMake(0, 0, thumbSize, thumbSize);
    self.thumb.layer.cornerRadius = thumbSize / 2;
    if (CGPointEqualToPoint(self.thumb.center, CGPointZero))
        self.thumb.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
}

- (void)updateTouch:(UITouch *)touch {
    if (self.layoutEditing || !g_virtual_joystick)
        return;
    const CGPoint center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    CGPoint point = [touch locationInView:self];
    CGFloat dx = point.x - center.x;
    CGFloat dy = point.y - center.y;
    const CGFloat radius = CGRectGetWidth(self.bounds) * 0.38;
    const CGFloat distance = hypot(dx, dy);
    if (distance > radius) {
        dx *= radius / distance;
        dy *= radius / distance;
    }
    self.thumb.center = CGPointMake(center.x + dx, center.y + dy);
    SDL_SetJoystickVirtualAxis(g_virtual_joystick, static_cast<int>(self.horizontalAxis),
        static_cast<Sint16>(std::clamp(dx / radius, -1.0, 1.0) * 32767));
    SDL_SetJoystickVirtualAxis(g_virtual_joystick, static_cast<int>(self.verticalAxis),
        static_cast<Sint16>(std::clamp(dy / radius, -1.0, 1.0) * 32767));
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    hapticTick();
    [self updateTouch:touches.anyObject];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self updateTouch:touches.anyObject];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)touches;
    (void)event;
    [self resetAxes];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}

- (void)resetAxes {
    self.thumb.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    if (g_virtual_joystick) {
        SDL_SetJoystickVirtualAxis(g_virtual_joystick, static_cast<int>(self.horizontalAxis), 0);
        SDL_SetJoystickVirtualAxis(g_virtual_joystick, static_cast<int>(self.verticalAxis), 0);
    }
}

@end


@interface Vita3KVirtualControllerView : UIView
@property(nonatomic, strong) NSMutableArray<UIView *> *controllerElements;
@property(nonatomic, strong) UIButton *menuButton;
@property(nonatomic, strong) UIButton *editDoneButton;
@property(nonatomic, strong) UIView *verticalGuide;
@property(nonatomic, strong) UIView *horizontalGuide;
@property(nonatomic) BOOL layoutEditing;
// YES when the overlay was created solely to edit the layout from the homepage
// (no game running); it is torn down again when editing finishes.
@property(nonatomic) BOOL previewEditingOnly;
@property(nonatomic, strong) NSTimer *menuFadeTimer;
- (void)applyConfiguration;
- (void)setLayoutEditing:(BOOL)editing;
- (void)releaseAllInputs;
@end


static void dismissGameMenu();
static void presentGameMenu();
static UITapGestureRecognizer *g_three_finger_tap = nil;

@implementation Vita3KVirtualControllerView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    loadConfig();
    self.backgroundColor = UIColor.clearColor;
    self.multipleTouchEnabled = YES;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.controllerElements = [NSMutableArray array];

    [self addButton:@"△" key:@"triangle" button:SDL_GAMEPAD_BUTTON_NORTH accessibility:@"Triangle"];
    [self addButton:@"○" key:@"circle" button:SDL_GAMEPAD_BUTTON_EAST accessibility:@"Circle"];
    [self addButton:@"×" key:@"cross" button:SDL_GAMEPAD_BUTTON_SOUTH accessibility:@"Cross"];
    [self addButton:@"□" key:@"square" button:SDL_GAMEPAD_BUTTON_WEST accessibility:@"Square"];
    [self addButton:@"↑" key:@"dpad_up" button:SDL_GAMEPAD_BUTTON_DPAD_UP accessibility:@"D-pad up"];
    [self addButton:@"↓" key:@"dpad_down" button:SDL_GAMEPAD_BUTTON_DPAD_DOWN accessibility:@"D-pad down"];
    [self addButton:@"←" key:@"dpad_left" button:SDL_GAMEPAD_BUTTON_DPAD_LEFT accessibility:@"D-pad left"];
    [self addButton:@"→" key:@"dpad_right" button:SDL_GAMEPAD_BUTTON_DPAD_RIGHT accessibility:@"D-pad right"];
    [self addButton:@"L" key:@"left_shoulder" button:SDL_GAMEPAD_BUTTON_LEFT_SHOULDER accessibility:@"Left shoulder"];
    [self addButton:@"R" key:@"right_shoulder" button:SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER accessibility:@"Right shoulder"];
    [self addButton:@"SELECT" key:@"select" button:SDL_GAMEPAD_BUTTON_BACK accessibility:@"Select"];
    [self addButton:@"START" key:@"start" button:SDL_GAMEPAD_BUTTON_START accessibility:@"Start"];
    [self addStick:@"left_stick" horizontal:SDL_GAMEPAD_AXIS_LEFTX vertical:SDL_GAMEPAD_AXIS_LEFTY accessibility:@"Left analog stick"];
    [self addStick:@"right_stick" horizontal:SDL_GAMEPAD_AXIS_RIGHTX vertical:SDL_GAMEPAD_AXIS_RIGHTY accessibility:@"Right analog stick"];

    self.menuButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.menuButton.accessibilityIdentifier = @"menu";
    self.menuButton.accessibilityLabel = @"In-game menu";
    [self.menuButton setImage:[UIImage systemImageNamed:@"ellipsis"] forState:UIControlStateNormal];
    self.menuButton.tintColor = UIColor.labelColor;
    self.menuButton.backgroundColor = UIColor.clearColor;
    self.menuButton.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.55].CGColor;
    self.menuButton.layer.borderWidth = 1;
    [self.menuButton addTarget:self action:@selector(menuTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.menuButton addTarget:self action:@selector(resetMenuFade) forControlEvents:UIControlEventTouchDown];
    installGlassBackground(self.menuButton);
    [self addSubview:self.menuButton];

    self.verticalGuide = [[UIView alloc] init];
    self.verticalGuide.backgroundColor = UIColor.systemCyanColor;
    self.verticalGuide.hidden = YES;
    [self addSubview:self.verticalGuide];
    self.horizontalGuide = [[UIView alloc] init];
    self.horizontalGuide.backgroundColor = UIColor.systemCyanColor;
    self.horizontalGuide.hidden = YES;
    [self addSubview:self.horizontalGuide];

    self.editDoneButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.editDoneButton setTitle:@"Done Editing" forState:UIControlStateNormal];
    self.editDoneButton.backgroundColor = UIColor.systemBlueColor;
    [self.editDoneButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.editDoneButton.layer.cornerRadius = 18;
    self.editDoneButton.hidden = YES;
    [self.editDoneButton addTarget:self action:@selector(finishEditing) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.editDoneButton];

    for (UIView *elementView in self.controllerElements) {
        UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(elementPanned:)];
        pan.enabled = NO;
        [elementView addGestureRecognizer:pan];
    }
    UIPanGestureRecognizer *menuPan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(menuPanned:)];
    [self.menuButton addGestureRecognizer:menuPan];
    [self resetMenuFade];
    return self;
}

- (void)willMoveToWindow:(UIWindow *)newWindow {
    if (!newWindow)
        [self.menuFadeTimer invalidate];
    [super willMoveToWindow:newWindow];
}

- (void)resetMenuFade {
    [self.menuFadeTimer invalidate];
    self.menuButton.alpha = 1.0;
    if (self.menuButton.hidden || self.layoutEditing)
        return;
    self.menuFadeTimer = [NSTimer scheduledTimerWithTimeInterval:2.75 target:self
        selector:@selector(fadeMenuButton) userInfo:nil repeats:NO];
}

- (void)fadeMenuButton {
    [UIView animateWithDuration:0.55 animations:^{ self.menuButton.alpha = 0.12; }];
}

- (void)addButton:(NSString *)title key:(NSString *)key button:(SDL_GamepadButton)gamepadButton accessibility:(NSString *)accessibility {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.tag = static_cast<NSInteger>(gamepadButton);
    button.accessibilityIdentifier = key;
    button.accessibilityLabel = accessibility;
    button.multipleTouchEnabled = YES;
    button.exclusiveTouch = NO;
    button.backgroundColor = UIColor.clearColor;
    button.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.62].CGColor;
    button.layer.borderWidth = 1.25;
    // SELECT/START carry a whole word inside a small capsule; use a smaller
    // font so the label fits instead of hugging the edges.
    const BOOL wordLabel = [key isEqualToString:@"select"] || [key isEqualToString:@"start"];
    button.titleLabel.font = [UIFont systemFontOfSize:wordLabel ? 10 : 17 weight:UIFontWeightBold];
    button.titleLabel.adjustsFontSizeToFitWidth = YES;
    button.titleLabel.minimumScaleFactor = 0.7;
    [button setTitle:title forState:UIControlStateNormal];
    [button setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [button setTitleColor:UIColor.systemCyanColor forState:UIControlStateHighlighted];
    [button addTarget:self action:@selector(buttonPressed:) forControlEvents:UIControlEventTouchDown | UIControlEventTouchDragEnter];
    [button addTarget:self action:@selector(buttonReleased:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel | UIControlEventTouchDragExit];
    installGlassBackground(button);
    [self addSubview:button];
    [self.controllerElements addObject:button];
}

- (void)addStick:(NSString *)key horizontal:(SDL_GamepadAxis)horizontal vertical:(SDL_GamepadAxis)vertical accessibility:(NSString *)accessibility {
    Vita3KAnalogStick *stick = [[Vita3KAnalogStick alloc] init];
    stick.elementIdentifier = key;
    stick.accessibilityIdentifier = key;
    stick.accessibilityLabel = accessibility;
    stick.horizontalAxis = horizontal;
    stick.verticalAxis = vertical;
    [self addSubview:stick];
    [self.controllerElements addObject:stick];
}

- (void)buttonPressed:(UIButton *)sender {
    if (self.layoutEditing)
        return;
    hapticTick();
    sender.backgroundColor = [UIColor colorWithRed:0.2 green:0.65 blue:1 alpha:0.28];
    if (g_virtual_joystick)
        SDL_SetJoystickVirtualButton(g_virtual_joystick, static_cast<int>(sender.tag), true);
}

- (void)buttonReleased:(UIButton *)sender {
    sender.backgroundColor = UIColor.clearColor;
    if (g_virtual_joystick)
        SDL_SetJoystickVirtualButton(g_virtual_joystick, static_cast<int>(sender.tag), false);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat nativeScale = self.window.screen.nativeScale > 0
        ? self.window.screen.nativeScale : UIScreen.mainScreen.scale;
    g_safe_area_top_pixels.store(static_cast<float>(safe.top * nativeScale), std::memory_order_release);
    self.verticalGuide.frame = CGRectMake(CGRectGetMidX(self.bounds), safe.top, 1, CGRectGetHeight(self.bounds) - safe.top - safe.bottom);
    self.horizontalGuide.frame = CGRectMake(safe.left, CGRectGetMidY(self.bounds), CGRectGetWidth(self.bounds) - safe.left - safe.right, 1);
    self.editDoneButton.frame = CGRectMake(CGRectGetMidX(self.bounds) - 68, safe.top + 10, 136, 36);
    [self applyConfiguration];
}

- (CGSize)sizeForElement:(UIView *)elementView scale:(CGFloat)scale {
    NSString *key = elementView.accessibilityIdentifier;
    if ([elementView isKindOfClass:Vita3KAnalogStick.class])
        return CGSizeMake(96 * scale, 96 * scale);
    if ([key containsString:@"shoulder"])
        return CGSizeMake(94 * scale, 42 * scale);
    if ([key isEqualToString:@"select"] || [key isEqualToString:@"start"])
        return CGSizeMake(76 * scale, 34 * scale);
    return CGSizeMake(58 * scale, 58 * scale);
}

- (void)applyConfiguration {
    loadConfig();
    const CGFloat width = CGRectGetWidth(self.bounds);
    const CGFloat height = CGRectGetHeight(self.bounds);
    const CGFloat scale = [g_controls_config[@"scale"] doubleValue];
    const CGFloat opacity = [g_controls_config[@"opacity"] doubleValue];
    const BOOL hideForPhysical = [g_controls_config[@"hideWhenPhysical"] boolValue]
        && g_physical_controller_connected.load(std::memory_order_relaxed);
    for (UIView *elementView in self.controllerElements) {
        NSMutableDictionary *settings = elementConfig(elementView.accessibilityIdentifier);
        elementView.hidden = hideForPhysical || ![settings[@"visible"] boolValue];
        elementView.alpha = opacity;
        const CGSize size = [self sizeForElement:elementView scale:scale];
        elementView.bounds = (CGRect){CGPointZero, size};
        elementView.center = CGPointMake([settings[@"x"] doubleValue] * width, [settings[@"y"] doubleValue] * height);
        elementView.layer.cornerRadius = MIN(size.width, size.height) / 2;
        layoutGlassBackground(elementView);
    }
    NSMutableDictionary *menuSettings = elementConfig(@"menu");
    self.menuButton.hidden = ![menuSettings[@"visible"] boolValue];
    self.menuButton.bounds = CGRectMake(0, 0, 46, 46);
    const CGFloat half = 23;
    self.menuButton.center = CGPointMake(
        std::clamp(static_cast<CGFloat>([menuSettings[@"x"] doubleValue] * width), half, width - half),
        std::clamp(static_cast<CGFloat>([menuSettings[@"y"] doubleValue] * height), half, height - half));
    self.menuButton.layer.cornerRadius = 23;
    layoutGlassBackground(self.menuButton);
}

- (void)setLayoutEditing:(BOOL)editing {
    _layoutEditing = editing;
    self.editDoneButton.hidden = !editing;
    self.menuButton.hidden = editing;
    for (UIView *elementView in self.controllerElements) {
        elementView.hidden = editing ? ![elementConfig(elementView.accessibilityIdentifier)[@"visible"] boolValue] : elementView.hidden;
        elementView.layer.borderColor = editing ? UIColor.systemCyanColor.CGColor : [UIColor colorWithWhite:1 alpha:0.62].CGColor;
        for (UIGestureRecognizer *recognizer in elementView.gestureRecognizers)
            recognizer.enabled = editing;
        if ([elementView isKindOfClass:Vita3KAnalogStick.class])
            ((Vita3KAnalogStick *)elementView).layoutEditing = editing;
    }
    if (!editing)
        [self applyConfiguration];
}

- (void)elementPanned:(UIPanGestureRecognizer *)recognizer {
    UIView *elementView = recognizer.view;
    const CGPoint translation = [recognizer translationInView:self];
    elementView.center = CGPointMake(elementView.center.x + translation.x, elementView.center.y + translation.y);
    [recognizer setTranslation:CGPointZero inView:self];
    self.verticalGuide.hidden = recognizer.state == UIGestureRecognizerStateEnded || recognizer.state == UIGestureRecognizerStateCancelled;
    self.horizontalGuide.hidden = self.verticalGuide.hidden;
    if (recognizer.state == UIGestureRecognizerStateEnded) {
        // Save the exact normalized position (no grid snapping) so elements
        // stay exactly where they were dropped; only clamp to keep them fully
        // on screen. The center guides above are unaffected.
        CGFloat x = std::clamp(static_cast<CGFloat>(elementView.center.x / CGRectGetWidth(self.bounds)), 0.04, 0.96);
        CGFloat y = std::clamp(static_cast<CGFloat>(elementView.center.y / CGRectGetHeight(self.bounds)), 0.06, 0.94);
        NSMutableDictionary *settings = elementConfig(elementView.accessibilityIdentifier);
        settings[@"x"] = @(x);
        settings[@"y"] = @(y);
        elementView.center = CGPointMake(x * CGRectGetWidth(self.bounds), y * CGRectGetHeight(self.bounds));
        saveConfig();
        hapticTick();
    }
}

- (void)menuPanned:(UIPanGestureRecognizer *)recognizer {
    if (self.layoutEditing)
        return;
    if (recognizer.state == UIGestureRecognizerStateBegan)
        [self resetMenuFade];
    const CGPoint translation = [recognizer translationInView:self];
    self.menuButton.center = CGPointMake(self.menuButton.center.x + translation.x, self.menuButton.center.y + translation.y);
    [recognizer setTranslation:CGPointZero inView:self];
    if (recognizer.state == UIGestureRecognizerStateEnded) {
        const CGFloat half = CGRectGetWidth(self.menuButton.bounds) / 2;
        const CGFloat left = half;
        const CGFloat right = CGRectGetWidth(self.bounds) - half;
        self.menuButton.center = CGPointMake(self.menuButton.center.x < CGRectGetMidX(self.bounds) ? left : right,
            std::clamp(self.menuButton.center.y, half, CGRectGetHeight(self.bounds) - half));
        NSMutableDictionary *settings = elementConfig(@"menu");
        settings[@"x"] = @(self.menuButton.center.x / CGRectGetWidth(self.bounds));
        settings[@"y"] = @(self.menuButton.center.y / CGRectGetHeight(self.bounds));
        saveConfig();
        [self resetMenuFade];
    }
}

- (void)menuTapped {
    [self resetMenuFade];
    if (!self.layoutEditing)
        presentGameMenu();
}

- (void)threeFingerTap {
    // Restores the in-game menu button after "Hide Menu Button".
    NSMutableDictionary *settings = elementConfig(@"menu");
    if ([settings[@"visible"] boolValue])
        return;
    settings[@"visible"] = @YES;
    saveConfig();
    [self applyConfiguration];
    [self resetMenuFade];
    hapticTick();
}

- (void)finishEditing {
    [self setLayoutEditing:NO];
    saveConfig();
    if (self.previewEditingOnly) {
        // Homepage edit session: remove the preview overlay so it does not sit
        // on top of the library once editing is done.
        [self removeFromSuperview];
        if (g_overlay == self)
            g_overlay = nil;
    }
}

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    if (!self.editDoneButton.hidden && [self.editDoneButton pointInside:[self.editDoneButton convertPoint:point fromView:self] withEvent:event])
        return YES;
    if (!self.menuButton.hidden && [self.menuButton pointInside:[self.menuButton convertPoint:point fromView:self] withEvent:event])
        return YES;
    for (UIView *elementView in self.controllerElements) {
        if (!elementView.hidden && [elementView pointInside:[elementView convertPoint:point fromView:self] withEvent:event])
            return YES;
    }
    return NO;
}

- (void)releaseAllInputs {
    if (!g_virtual_joystick)
        return;
    for (UIView *elementView in self.controllerElements) {
        if ([elementView isKindOfClass:UIButton.class])
            SDL_SetJoystickVirtualButton(g_virtual_joystick, static_cast<int>(((UIButton *)elementView).tag), false);
        else if ([elementView isKindOfClass:Vita3KAnalogStick.class])
            [(Vita3KAnalogStick *)elementView resetAxes];
    }
}

@end


@interface Vita3KControllerOptionsView : UIControl
@property(nonatomic, strong) UISlider *opacitySlider;
@property(nonatomic, strong) UISlider *scaleSlider;
@end

@implementation Vita3KControllerOptionsView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    loadConfig();
    self.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    UIVisualEffectView *glass = [[UIVisualEffectView alloc] initWithEffect:glassEffect()];
    glass.translatesAutoresizingMaskIntoConstraints = NO;
    glass.layer.cornerRadius = 28;
    glass.clipsToBounds = YES;
    [self addSubview:glass];
    // Size against the safe-area width so the panel never slips under the
    // dynamic island / notch in landscape.
    NSLayoutConstraint *preferredWidth = [glass.widthAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.widthAnchor multiplier:0.92];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [glass.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [glass.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [glass.widthAnchor constraintLessThanOrEqualToConstant:560],
        preferredWidth,
        [glass.heightAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.heightAnchor multiplier:0.86],
        [glass.heightAnchor constraintLessThanOrEqualToAnchor:self.heightAnchor multiplier:0.88],
    ]];

    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [glass.contentView addSubview:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:glass.contentView.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:glass.contentView.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:glass.contentView.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:glass.contentView.bottomAnchor],
    ]];
    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.layoutMargins = UIEdgeInsetsMake(22, 22, 22, 22);
    stack.layoutMarginsRelativeArrangement = YES;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
    ]];

    UILabel *title = [[UILabel alloc] init];
    title.text = @"Virtual Controls";
    title.font = [UIFont systemFontOfSize:27 weight:UIFontWeightBold];
    title.textColor = UIColor.labelColor;
    [stack addArrangedSubview:title];

    self.opacitySlider = [[UISlider alloc] init];
    self.opacitySlider.minimumValue = 0.15;
    self.opacitySlider.maximumValue = 1.0;
    self.opacitySlider.value = [g_controls_config[@"opacity"] floatValue];
    [self.opacitySlider addTarget:self action:@selector(opacityChanged:) forControlEvents:UIControlEventValueChanged];
    [stack addArrangedSubview:[self labeled:@"Opacity" control:self.opacitySlider]];
    self.scaleSlider = [[UISlider alloc] init];
    self.scaleSlider.minimumValue = 0.65;
    self.scaleSlider.maximumValue = 1.45;
    self.scaleSlider.value = [g_controls_config[@"scale"] floatValue];
    [self.scaleSlider addTarget:self action:@selector(scaleChanged:) forControlEvents:UIControlEventValueChanged];
    [stack addArrangedSubview:[self labeled:@"Element scale" control:self.scaleSlider]];

    UISwitch *autoHide = [[UISwitch alloc] init];
    autoHide.on = [g_controls_config[@"hideWhenPhysical"] boolValue];
    autoHide.accessibilityIdentifier = @"hideWhenPhysical";
    [autoHide addTarget:self action:@selector(optionSwitch:) forControlEvents:UIControlEventValueChanged];
    [stack addArrangedSubview:[self labeled:@"Hide for physical controller" control:autoHide]];
    UISwitch *haptics = [[UISwitch alloc] init];
    haptics.on = [g_controls_config[@"haptics"] boolValue];
    haptics.accessibilityIdentifier = @"haptics";
    [haptics addTarget:self action:@selector(optionSwitch:) forControlEvents:UIControlEventValueChanged];
    [stack addArrangedSubview:[self labeled:@"Haptic ticks" control:haptics]];

    UILabel *visibility = [[UILabel alloc] init];
    visibility.text = @"VISIBLE ELEMENTS";
    visibility.font = [UIFont systemFontOfSize:12 weight:UIFontWeightBold];
    visibility.textColor = UIColor.systemPinkColor;
    [stack addArrangedSubview:visibility];
    NSArray<NSArray<NSString *> *> *elements = @[
        @[@"D-pad up", @"dpad_up"], @[@"D-pad down", @"dpad_down"], @[@"D-pad left", @"dpad_left"], @[@"D-pad right", @"dpad_right"],
        @[@"Triangle", @"triangle"], @[@"Circle", @"circle"], @[@"Cross", @"cross"], @[@"Square", @"square"],
        @[@"L shoulder", @"left_shoulder"], @[@"R shoulder", @"right_shoulder"], @[@"Select", @"select"], @[@"Start", @"start"],
        @[@"Left stick", @"left_stick"], @[@"Right stick", @"right_stick"], @[@"Menu button", @"menu"],
    ];
    for (NSArray<NSString *> *item in elements) {
        UISwitch *toggle = [[UISwitch alloc] init];
        toggle.on = [elementConfig(item[1])[@"visible"] boolValue];
        toggle.accessibilityIdentifier = item[1];
        [toggle addTarget:self action:@selector(elementSwitch:) forControlEvents:UIControlEventValueChanged];
        [stack addArrangedSubview:[self labeled:item[0] control:toggle]];
    }

    UIButton *edit = [self actionButton:@"Edit Layout" color:UIColor.systemBlueColor selector:@selector(editLayout)];
    UIButton *reset = [self actionButton:@"Reset to Default" color:UIColor.systemOrangeColor selector:@selector(resetLayout)];
    UIButton *done = [self actionButton:@"Done" color:UIColor.systemGrayColor selector:@selector(close)];
    [stack addArrangedSubview:edit];
    [stack addArrangedSubview:reset];
    [stack addArrangedSubview:done];
    return self;
}

- (UIView *)labeled:(NSString *)label control:(UIView *)control {
    UILabel *title = [[UILabel alloc] init];
    title.text = label;
    title.textColor = UIColor.labelColor;
    title.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[title, control]];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 12;
    [title setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return row;
}

- (UIButton *)actionButton:(NSString *)title color:(UIColor *)color selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:title forState:UIControlStateNormal];
    [button setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    button.backgroundColor = [color colorWithAlphaComponent:0.75];
    button.layer.cornerRadius = 17;
    [button.heightAnchor constraintEqualToConstant:44].active = YES;
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)opacityChanged:(UISlider *)slider {
    g_controls_config[@"opacity"] = @(slider.value);
    [g_overlay applyConfiguration];
    saveConfig();
}
- (void)scaleChanged:(UISlider *)slider {
    g_controls_config[@"scale"] = @(slider.value);
    [g_overlay applyConfiguration];
    saveConfig();
}
- (void)optionSwitch:(UISwitch *)sender {
    g_controls_config[sender.accessibilityIdentifier] = @(sender.on);
    [g_overlay applyConfiguration];
    saveConfig();
}
- (void)elementSwitch:(UISwitch *)sender {
    elementConfig(sender.accessibilityIdentifier)[@"visible"] = @(sender.on);
    [g_overlay applyConfiguration];
    saveConfig();
}
- (void)editLayout {
    [self close];
    if (!g_overlay) {
        // Invoked from the homepage with no game running, so there is no live
        // overlay yet. Spin one up purely for editing; finishEditing tears it
        // back down. No SDL joystick is needed to reposition controls.
        UIWindow *window = activeWindow();
        if (!window)
            return;
        Vita3KVirtualControllerView *overlay = [[Vita3KVirtualControllerView alloc] initWithFrame:window.bounds];
        overlay.previewEditingOnly = YES;
        g_overlay = overlay;
        [window addSubview:overlay];
        [window bringSubviewToFront:overlay];
        // Lay out once so elements are positioned before edit borders/handles
        // turn on (and the menu button is hidden for good).
        [overlay layoutIfNeeded];
    }
    [g_overlay setLayoutEditing:YES];
}
- (void)resetLayout {
    NSMutableDictionary *defaults = defaultConfig();
    NSString *key = orientationKey();
    g_controls_config[@"layouts"][key] = defaults[@"layouts"][key];
    saveConfig();
    [g_overlay applyConfiguration];
    [self close];
    vita3k_ios_present_controller_options();
}
- (void)close {
    [UIView animateWithDuration:0.2 animations:^{ self.alpha = 0; } completion:^(__unused BOOL finished) {
        [self removeFromSuperview];
        g_options_view = nil;
    }];
}

@end


static UIButton *menuAction(NSString *title, NSString *subtitle, NSString *symbol, SEL selector, id target, BOOL destructive = NO) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    UIColor *foreground = destructive ? UIColor.systemRedColor : UIColor.labelColor;
    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.contentMode = UIViewContentModeCenter;
    icon.tintColor = foreground;
    icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:21 weight:UIImageSymbolWeightMedium];
    icon.userInteractionEnabled = NO;

    UILabel *titleLabel = [[UILabel alloc] init];
    titleLabel.text = title;
    titleLabel.textColor = foreground;
    titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    UILabel *subtitleLabel = [[UILabel alloc] init];
    subtitleLabel.text = subtitle;
    subtitleLabel.textColor = destructive ? [UIColor.systemRedColor colorWithAlphaComponent:0.85]
                                          : UIColor.secondaryLabelColor;
    subtitleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    subtitleLabel.numberOfLines = 1;
    UIStackView *labels = subtitle.length
        ? [[UIStackView alloc] initWithArrangedSubviews:@[titleLabel, subtitleLabel]]
        : [[UIStackView alloc] initWithArrangedSubviews:@[titleLabel]];
    labels.translatesAutoresizingMaskIntoConstraints = NO;
    labels.axis = UILayoutConstraintAxisVertical;
    labels.spacing = 1;
    labels.alignment = UIStackViewAlignmentLeading;
    labels.userInteractionEnabled = NO;
    [button addSubview:icon];
    [button addSubview:labels];
    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:6],
        [icon.centerYAnchor constraintEqualToAnchor:button.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:34],
        [icon.heightAnchor constraintEqualToConstant:34],
        [labels.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:54],
        [labels.trailingAnchor constraintLessThanOrEqualToAnchor:button.trailingAnchor constant:-6],
        [labels.centerYAnchor constraintEqualToAnchor:button.centerYAnchor],
    ]];
    button.accessibilityLabel = title;
    button.accessibilityHint = subtitle;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:58].active = YES;
    [button addTarget:target action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

@interface Vita3KGameMenuTarget : NSObject
@end
@implementation Vita3KGameMenuTarget
- (void)resume { dismissGameMenu(); }
- (void)layout { dismissGameMenu(); vita3k_ios_present_controller_options(); }
- (void)trophies { dismissGameMenu(); vita3k_ios_request_current_trophies(); }
- (void)togglePerfHud {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults setBool:![defaults boolForKey:@"vita3k.perf.hidden"] forKey:@"vita3k.perf.hidden"];
    dismissGameMenu();
}
- (void)hideMenuButton {
    elementConfig(@"menu")[@"visible"] = @NO;
    saveConfig();
    [g_overlay applyConfiguration];
    dismissGameMenu();
}
- (void)quit {
    dismissGameMenu();
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}
@end
static Vita3KGameMenuTarget *g_game_menu_target = nil;

static void presentGameMenu() {
    if (g_game_menu)
        return;
    UIWindow *window = activeWindow();
    if (!window)
        return;
    g_game_menu_target = [[Vita3KGameMenuTarget alloc] init];
    UIControl *shade = [[UIControl alloc] initWithFrame:window.bounds];
    shade.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    shade.backgroundColor = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark
            ? [UIColor colorWithWhite:0 alpha:0.40]
            : [UIColor colorWithWhite:1 alpha:0.32];
    }];
    [shade addTarget:g_game_menu_target action:@selector(resume) forControlEvents:UIControlEventTouchUpInside];
    UIVisualEffectView *glass = [[UIVisualEffectView alloc] initWithEffect:glassEffect()];
    glass.translatesAutoresizingMaskIntoConstraints = NO;
    glass.layer.cornerRadius = 28;
    glass.clipsToBounds = YES;
    [shade addSubview:glass];
    const UIEdgeInsets safe = window.safeAreaInsets;
    const CGFloat panelHeight = MIN(470, MAX(280, CGRectGetHeight(window.bounds) - safe.top - safe.bottom - 24));
    NSLayoutConstraint *preferredWidth = [glass.widthAnchor constraintEqualToAnchor:shade.safeAreaLayoutGuide.widthAnchor multiplier:0.92];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [glass.centerXAnchor constraintEqualToAnchor:shade.centerXAnchor],
        [glass.bottomAnchor constraintEqualToAnchor:shade.safeAreaLayoutGuide.bottomAnchor constant:-12],
        [glass.leadingAnchor constraintGreaterThanOrEqualToAnchor:shade.safeAreaLayoutGuide.leadingAnchor constant:12],
        [glass.trailingAnchor constraintLessThanOrEqualToAnchor:shade.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [glass.widthAnchor constraintLessThanOrEqualToConstant:410],
        preferredWidth,
        [glass.heightAnchor constraintEqualToConstant:panelHeight],
    ]];
    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [glass.contentView addSubview:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:glass.contentView.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:glass.contentView.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:glass.contentView.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:glass.contentView.bottomAnchor],
    ]];
    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 5;
    stack.layoutMargins = UIEdgeInsetsMake(18, 18, 20, 18);
    stack.layoutMarginsRelativeArrangement = YES;
    [scroll addSubview:stack];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
    ]];
    UILabel *title = [[UILabel alloc] init];
    title.text = @"Game Menu";
    title.font = [UIFont systemFontOfSize:25 weight:UIFontWeightBold];
    title.textColor = UIColor.labelColor;
    [stack addArrangedSubview:title];
    UILabel *subtitle = [[UILabel alloc] init];
    subtitle.text = @"Quick actions";
    subtitle.textColor = UIColor.secondaryLabelColor;
    subtitle.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    [stack addArrangedSubview:subtitle];
    [stack addArrangedSubview:menuAction(@"Resume", nil, @"play.fill", @selector(resume), g_game_menu_target)];
    [stack addArrangedSubview:menuAction(@"Trophies", @"Progress and unlock dates", @"trophy.fill", @selector(trophies), g_game_menu_target)];
    [stack addArrangedSubview:menuAction(@"Controller Options", @"Layout, visibility, scale and opacity", @"gamecontroller.fill", @selector(layout), g_game_menu_target)];
    NSString *hudTitle = [NSUserDefaults.standardUserDefaults boolForKey:@"vita3k.perf.hidden"]
        ? @"Show Performance HUD"
        : @"Hide Performance HUD";
    [stack addArrangedSubview:menuAction(hudTitle, @"FPS, memory and battery overlay", @"gauge.with.dots.needle.67percent",
        @selector(togglePerfHud), g_game_menu_target)];
    [stack addArrangedSubview:menuAction(@"Hide Menu Button", @"Restore it with a three-finger tap", @"eye.slash.fill",
        @selector(hideMenuButton), g_game_menu_target)];
    [stack addArrangedSubview:menuAction(@"Quit Game", @"Return to the library", @"rectangle.portrait.and.arrow.right",
        @selector(quit), g_game_menu_target, YES)];
    g_game_menu = shade;
    [window addSubview:shade];
}

static void dismissGameMenu() {
    [g_game_menu removeFromSuperview];
    g_game_menu = nil;
    g_game_menu_target = nil;
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
    SDL_Log("Vita3K iOS: virtual touch controller attached (joystick=%u, dual analog enabled)",
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
        UITapGestureRecognizer *restore = [[UITapGestureRecognizer alloc] initWithTarget:g_overlay
                                                                                  action:@selector(threeFingerTap)];
        restore.numberOfTouchesRequired = 3;
        restore.cancelsTouchesInView = NO;
        [window addGestureRecognizer:restore];
        g_three_finger_tap = restore;
        SDL_Log("Vita3K iOS: virtual touch controller visible (layout=%s)", configPath().UTF8String);
    });
}

void vita3k_ios_hide_virtual_controller() {
    performOnMainThread(^{
        [g_three_finger_tap.view removeGestureRecognizer:g_three_finger_tap];
        g_three_finger_tap = nil;
        [g_overlay releaseAllInputs];
        dismissGameMenu();
        [g_options_view removeFromSuperview];
        g_options_view = nil;
        [g_overlay removeFromSuperview];
        g_overlay = nil;
    });
}

void vita3k_ios_present_controller_options() {
    performOnMainThread(^{
        if (g_options_view)
            return;
        UIWindow *window = activeWindow();
        if (!window) {
            SDL_Log("Vita3K iOS: controller options presentation failed: no active window");
            return;
        }
        Vita3KControllerOptionsView *options = [[Vita3KControllerOptionsView alloc] initWithFrame:window.bounds];
        g_options_view = options;
        [window addSubview:options];
        [window bringSubviewToFront:options];
        SDL_Log("Vita3K iOS: controller options visible (orientation=%s)", orientationKey().UTF8String);
    });
}

float vita3k_ios_safe_area_top_pixels() {
    return g_safe_area_top_pixels.load(std::memory_order_acquire);
}

void vita3k_ios_set_physical_controller_connected(bool connected) {
    g_physical_controller_connected.store(connected, std::memory_order_relaxed);
    performOnMainThread(^{ [g_overlay applyConfiguration]; });
}

void vita3k_ios_detach_virtual_controller() {
    if (g_virtual_joystick)
        SDL_CloseJoystick(g_virtual_joystick);
    if (g_virtual_joystick_id != 0)
        SDL_DetachVirtualJoystick(g_virtual_joystick_id);
    g_virtual_joystick = nullptr;
    g_virtual_joystick_id = 0;
}
