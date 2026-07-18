// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#include <vita3k_ios/NativeFrontend.h>
#include <vita3k_ios/VirtualController.h>
#include <util/log.h>

// Apple's MacTypes.h declares `typedef char *Ptr;`, which collides with the
// emulator's global Ptr<T> template forward-declared by util/log.h above.
// Rename the MacTypes alias for the framework includes; nothing in this file
// uses it. MacTypes.h is fully included (and include-guarded) inside this
// region, so later transitive includes are no-ops.
#define Ptr MacTypesPtr
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach.h>
#undef Ptr

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

namespace {

std::mutex g_action_mutex;
std::optional<Vita3KIOSFrontendAction> g_pending_action;

void queue_action(Vita3KIOSFrontendAction action) {
    const std::lock_guard lock(g_action_mutex);
    g_pending_action = std::move(action);
}

void present_import_picker(BOOL firmware);
void present_license_picker();
void present_save_picker(NSString *titleId);
void reload_library_cells();

UIWindow *active_window() {
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

void perform_on_main(dispatch_block_t block) {
    if (NSThread.isMainThread)
        block();
    else
        dispatch_async(dispatch_get_main_queue(), block);
}

void present_alert(NSString *title, NSString *message) {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                  message:message
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [root presentViewController:alert animated:YES completion:nil];
}

// Per-title display-name override (a frontend-only rename), keyed by title id.
NSString *title_override_key(NSString *identifier) {
    return [@"tsubomi.title_override." stringByAppendingString:identifier ?: @""];
}

NSString *display_title(NSString *identifier, NSString *original) {
    NSString *override = [NSUserDefaults.standardUserDefaults stringForKey:title_override_key(identifier)];
    return override.length ? override : original;
}

// Whether library cells show the PCSG00291-style title id. Defaults ON.
BOOL show_title_ids() {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    return [defaults objectForKey:@"tsubomi.showTitleIds"] ? [defaults boolForKey:@"tsubomi.showTitleIds"] : YES;
}

NSString *tsubomi_app_version() {
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return version.length ? version : @"0.5.0";
}

NSString *game_metadata(const Vita3KIOSGameEntry &game) {
    NSByteCountFormatter *bytes = [[NSByteCountFormatter alloc] init];
    bytes.countStyle = NSByteCountFormatterCountStyleFile;
    NSString *size = [bytes stringFromByteCount:(long long)game.size_bytes];
    const long long minutes = MAX(0, game.time_played_seconds) / 60;
    NSString *played = game.time_played_seconds > 0 && minutes == 0
        ? @"<1m"
        : minutes >= 60
        ? [NSString stringWithFormat:@"%lldh %lldm", minutes / 60, minutes % 60]
        : [NSString stringWithFormat:@"%lldm", minutes];
    NSString *last = @"Never played";
    if (game.last_played_timestamp > 0) {
        NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
        formatter.dateStyle = NSDateFormatterShortStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
        last = [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:game.last_played_timestamp]];
    }
    NSString *versionText = [NSString stringWithUTF8String:game.version.c_str()];
    NSString *version = game.version.empty() || !versionText
        ? @"Unknown version"
        : [@"v" stringByAppendingString:versionText];
    return [NSString stringWithFormat:@"%@  ·  %@  ·  %@  ·  %@", version, played, last, size];
}

UIVisualEffect *glass_effect(const BOOL interactive = YES) {
    if (@available(iOS 26.0, *)) {
        UIGlassEffect *effect = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
        effect.interactive = interactive;
        // A subtle tint that follows the interface style: a fixed dark tint
        // washed the glass out in light mode.
        effect.tintColor = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
            return traits.userInterfaceStyle == UIUserInterfaceStyleDark
                ? [UIColor colorWithWhite:0.04 alpha:0.12]
                : [UIColor colorWithWhite:1.0 alpha:0.10];
        }];
        return effect;
    }
    // Adaptive material (not the ...Dark variant) so the pre-iOS 26 fallback
    // also tracks light/dark mode.
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterial];
}

// Plain toolbar glyph, no material behind it: header controls read as system
// bar buttons and adapt to light/dark through labelColor.
UIButton *symbol_button(NSString *symbol, NSString *fallback, NSString *accessibility) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.frame = CGRectMake(0, 0, 40, 40);
    UIImage *image = [UIImage systemImageNamed:symbol];
    if (image) {
        [button setPreferredSymbolConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:19 weight:UIImageSymbolWeightMedium]
                              forImageInState:UIControlStateNormal];
        [button setImage:image forState:UIControlStateNormal];
    } else {
        [button setTitle:fallback forState:UIControlStateNormal];
    }
    button.tintColor = UIColor.labelColor;
    button.accessibilityLabel = accessibility;
    return button;
}

std::string hex_bytes(const std::string &value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (const unsigned char byte : value) {
        if (!result.empty())
            result.push_back(' ');
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

} // namespace

@interface Vita3KGameCell : UICollectionViewCell
@property(nonatomic, strong) UIVisualEffectView *glass;
@property(nonatomic, strong) UIImageView *icon;
@property(nonatomic, strong) UILabel *titleLabel;
@property(nonatomic, strong) UILabel *identifierLabel;
@property(nonatomic, strong) UILabel *metadataLabel;
@property(nonatomic, strong) UIView *separator;
@property(nonatomic) BOOL listMode;
- (void)configureTitle:(NSString *)title identifier:(NSString *)identifier metadata:(NSString *)metadata
              iconPath:(NSString *)iconPath listMode:(BOOL)listMode;
@end

@implementation Vita3KGameCell

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    // Non-interactive glass: live refraction on every visible cell made the
    // library grid scroll stutter. Cells only need the static material.
    self.glass = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    self.glass.frame = self.contentView.bounds;
    self.glass.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.glass.layer.cornerRadius = 26;
    self.glass.clipsToBounds = YES;
    [self.contentView addSubview:self.glass];

    self.icon = [[UIImageView alloc] init];
    self.icon.contentMode = UIViewContentModeScaleAspectFill;
    self.icon.clipsToBounds = YES;
    self.icon.layer.cornerRadius = 18;
    [self.glass.contentView addSubview:self.icon];

    self.titleLabel = [[UILabel alloc] init];
    self.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.numberOfLines = 2;
    [self.glass.contentView addSubview:self.titleLabel];

    self.identifierLabel = [[UILabel alloc] init];
    self.identifierLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightMedium];
    self.identifierLabel.textColor = UIColor.secondaryLabelColor;
    [self.glass.contentView addSubview:self.identifierLabel];
    self.metadataLabel = [[UILabel alloc] init];
    self.metadataLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    self.metadataLabel.textColor = UIColor.secondaryLabelColor;
    self.metadataLabel.numberOfLines = 2;
    [self.glass.contentView addSubview:self.metadataLabel];
    self.separator = [[UIView alloc] init];
    self.separator.backgroundColor = UIColor.separatorColor;
    [self.glass.contentView addSubview:self.separator];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat inset = 12;
    if (self.listMode) {
        const CGFloat iconSize = MAX(1, CGRectGetHeight(self.bounds) - 20);
        self.icon.frame = CGRectMake(10, 10, iconSize, iconSize);
        const CGFloat textX = CGRectGetMaxX(self.icon.frame) + 13;
        const CGFloat textWidth = MAX(1, CGRectGetWidth(self.bounds) - textX - inset);
        self.titleLabel.frame = CGRectMake(textX, self.identifierLabel.hidden ? 12 : 6, textWidth, 24);
        self.identifierLabel.frame = CGRectMake(textX, CGRectGetMaxY(self.titleLabel.frame), textWidth, 17);
        self.metadataLabel.frame = CGRectMake(textX,
            self.identifierLabel.hidden ? CGRectGetMaxY(self.titleLabel.frame) + 7 : CGRectGetMaxY(self.identifierLabel.frame) + 3,
            textWidth, 28);
        self.separator.frame = CGRectMake(textX, CGRectGetHeight(self.bounds) - 1,
            MAX(1, CGRectGetWidth(self.bounds) - textX), 1);
    } else {
        const CGFloat labelHeight = self.identifierLabel.hidden ? 91 : 108;
        self.icon.frame = CGRectMake(inset, inset, CGRectGetWidth(self.bounds) - inset * 2,
            MAX(1, CGRectGetHeight(self.bounds) - labelHeight - inset * 2));
        self.titleLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.icon.frame) + 7,
            CGRectGetWidth(self.bounds) - inset * 2, 40);
        self.identifierLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.titleLabel.frame),
            CGRectGetWidth(self.bounds) - inset * 2, 17);
        self.metadataLabel.frame = CGRectMake(inset,
            self.identifierLabel.hidden ? CGRectGetMaxY(self.titleLabel.frame) + 3 : CGRectGetMaxY(self.identifierLabel.frame) + 2,
            CGRectGetWidth(self.bounds) - inset * 2, 34);
        self.separator.frame = CGRectZero;
    }
}

- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    [UIView animateWithDuration:0.16 animations:^{
        self.transform = highlighted ? CGAffineTransformMakeScale(0.96, 0.96) : CGAffineTransformIdentity;
        self.alpha = highlighted ? 0.78 : 1.0;
    }];
}

- (void)configureTitle:(NSString *)title identifier:(NSString *)identifier metadata:(NSString *)metadata
              iconPath:(NSString *)iconPath listMode:(BOOL)listMode {
    self.listMode = listMode;
    self.titleLabel.text = title;
    self.identifierLabel.text = identifier;
    self.metadataLabel.text = metadata;
    self.identifierLabel.hidden = !show_title_ids();
    self.separator.hidden = !listMode;
    // List rows sit directly on the background like a system list: no
    // material, no forced palette — every color adapts to light/dark mode.
    self.glass.effect = listMode ? nil : glass_effect(NO);
    self.glass.backgroundColor = UIColor.clearColor;
    self.glass.layer.cornerRadius = listMode ? 0 : 26;
    self.icon.layer.cornerRadius = listMode ? 8 : 18;
    self.titleLabel.numberOfLines = listMode ? 1 : 2;
    self.titleLabel.textColor = UIColor.labelColor;
    self.identifierLabel.textColor = UIColor.secondaryLabelColor;
    self.metadataLabel.textColor = UIColor.secondaryLabelColor;
    UIImage *image = iconPath.length ? [UIImage imageWithContentsOfFile:iconPath] : nil;
    if (iconPath.length && !image)
        LOG_ERROR("iOS library art could not be decoded at '{}'", iconPath.UTF8String);
    self.icon.image = image ?: [UIImage systemImageNamed:@"gamecontroller.fill"];
    self.icon.tintColor = UIColor.systemPinkColor;
    self.icon.backgroundColor = UIColor.secondarySystemFillColor;
    [self setNeedsLayout];
}

@end


@interface Vita3KSettingsView : UIView
@property(nonatomic) Vita3KIOSSettings values;
@property(nonatomic, strong) UIScrollView *scrollView;
@property(nonatomic, strong) UIStackView *stack;
@property(nonatomic, strong) UISlider *resolutionSlider;
@property(nonatomic, strong) UILabel *resolutionValue;
@property(nonatomic, strong) UISwitch *vsyncSwitch;
@property(nonatomic, strong) UISlider *fpsSlider;
@property(nonatomic, strong) UILabel *fpsValue;
@property(nonatomic, strong) UISwitch *cpuSwitch;
@property(nonatomic, strong) UISwitch *ngsSwitch;
@property(nonatomic, strong) UISwitch *asyncSwitch;
@property(nonatomic, strong) UISegmentedControl *anisotropicControl;
- (instancetype)initWithFrame:(CGRect)frame values:(const Vita3KIOSSettings &)values;
@end

@implementation Vita3KSettingsView

- (instancetype)initWithFrame:(CGRect)frame values:(const Vita3KIOSSettings &)values {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.values = values;
    // Adaptive base (white in light mode, near-black in dark) instead of the
    // old fixed navy; the settings panel now matches the rest of the UI.
    self.backgroundColor = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark ? UIColor.blackColor : UIColor.whiteColor;
    }];
    self.opaque = YES;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    self.scrollView = [[UIScrollView alloc] init];
    self.scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    self.scrollView.backgroundColor = self.backgroundColor;
    self.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    [self addSubview:self.scrollView];

    self.stack = [[UIStackView alloc] init];
    self.stack.axis = UILayoutConstraintAxisVertical;
    self.stack.spacing = 18;
    self.stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.scrollView addSubview:self.stack];
    [NSLayoutConstraint activateConstraints:@[
        [self.stack.leadingAnchor constraintEqualToAnchor:self.scrollView.frameLayoutGuide.leadingAnchor constant:20],
        [self.stack.trailingAnchor constraintEqualToAnchor:self.scrollView.frameLayoutGuide.trailingAnchor constant:-20],
        [self.stack.topAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.topAnchor constant:10],
        [self.stack.bottomAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.bottomAnchor constant:-30],
    ]];

    UIStackView *header = [[UIStackView alloc] init];
    header.translatesAutoresizingMaskIntoConstraints = NO;
    header.axis = UILayoutConstraintAxisHorizontal;
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 14; // keep the "Settings" title off the back chevron
    UIButton *back = symbol_button(@"chevron.left", @"Back", @"Back to library");
    [back addTarget:self action:@selector(close) forControlEvents:UIControlEventTouchUpInside];
    [back.widthAnchor constraintEqualToConstant:46].active = YES;
    [back.heightAnchor constraintEqualToConstant:46].active = YES;
    UILabel *title = [[UILabel alloc] init];
    title.text = @"Settings";
    title.font = [UIFont systemFontOfSize:32 weight:UIFontWeightBold];
    title.textColor = UIColor.labelColor;
    UIButton *save = [UIButton buttonWithType:UIButtonTypeSystem];
    [save setTitle:@"Save" forState:UIControlStateNormal];
    save.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [save addTarget:self action:@selector(save) forControlEvents:UIControlEventTouchUpInside];
    [header addArrangedSubview:back];
    [header addArrangedSubview:title];
    [header addArrangedSubview:save];
    [self addSubview:header];
    [NSLayoutConstraint activateConstraints:@[
        [header.leadingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor constant:20],
        [header.trailingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor constant:-20],
        [header.topAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.topAnchor constant:8],
        [header.heightAnchor constraintEqualToConstant:50],
        [self.scrollView.leadingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor],
        [self.scrollView.trailingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor],
        [self.scrollView.topAnchor constraintEqualToAnchor:header.bottomAnchor constant:8],
        [self.scrollView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];

    self.resolutionSlider = [[UISlider alloc] init];
    self.resolutionSlider.minimumValue = 1.0f;
    self.resolutionSlider.maximumValue = 4.0f;
    self.resolutionSlider.value = values.resolution_multiplier;
    [self.resolutionSlider addTarget:self action:@selector(resolutionChanged:) forControlEvents:UIControlEventValueChanged];
    self.resolutionValue = [[UILabel alloc] init];
    self.resolutionValue.textColor = UIColor.systemCyanColor;
    self.resolutionValue.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    [self updateResolutionLabel];
    UIStackView *resolutionAccessory = [[UIStackView alloc] initWithArrangedSubviews:@[self.resolutionSlider, self.resolutionValue]];
    resolutionAccessory.axis = UILayoutConstraintAxisHorizontal;
    resolutionAccessory.spacing = 10;
    resolutionAccessory.accessibilityIdentifier = @"wideAccessory";

    self.fpsSlider = [[UISlider alloc] init];
    self.fpsSlider.minimumValue = 15;
    self.fpsSlider.maximumValue = 60;
    self.fpsSlider.value = values.fps_limit;
    [self.fpsSlider addTarget:self action:@selector(fpsChanged:) forControlEvents:UIControlEventValueChanged];
    self.fpsValue = [[UILabel alloc] init];
    self.fpsValue.textColor = UIColor.systemCyanColor;
    self.fpsValue.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    UIStackView *fpsAccessory = [[UIStackView alloc] initWithArrangedSubviews:@[self.fpsSlider, self.fpsValue]];
    fpsAccessory.axis = UILayoutConstraintAxisHorizontal;
    fpsAccessory.spacing = 10;
    fpsAccessory.accessibilityIdentifier = @"wideAccessory";
    [self fpsChanged:self.fpsSlider];
    [self addSection:@"Video" rows:@[
        [self row:@"Resolution multiplier" hint:@"Higher values are sharper but increase GPU load." accessory:resolutionAccessory],
        [self switchRow:@"V-Sync" hint:@"Synchronizes presentation to the display." value:values.v_sync output:&_vsyncSwitch],
        [self row:@"FPS limiter" hint:@"Caps presentation without changing the Vita's 60 Hz timing." accessory:fpsAccessory],
    ]];

    self.anisotropicControl = [[UISegmentedControl alloc] initWithItems:@[@"Off", @"2x", @"4x", @"8x", @"16x"]];
    const int anisotropicValues[] = {1, 2, 4, 8, 16};
    self.anisotropicControl.selectedSegmentIndex = 0;
    for (NSInteger index = 0; index < 5; ++index) {
        if (values.anisotropic_filtering == anisotropicValues[index])
            self.anisotropicControl.selectedSegmentIndex = index;
    }
    [self addSection:@"Graphics" rows:@[
        [self switchRow:@"Async pipeline compilation" hint:@"Reduces shader stutter while new scenes compile." value:values.async_pipeline_compilation output:&_asyncSwitch],
        [self row:@"Anisotropic filtering" hint:@"Sharpens textures viewed at an angle." accessory:self.anisotropicControl],
    ]];

    [self addSection:@"Audio" rows:@[
        [self switchRow:@"NGS audio" hint:@"Full Vita audio emulation; disable only while diagnosing." value:values.ngs_enable output:&_ngsSwitch],
        [self row:@"Audio backend" hint:@"SDL is the supported iOS backend." accessory:[self valueLabel:@"SDL · iOS"]],
    ]];

    UIButton *controller = [UIButton buttonWithType:UIButtonTypeSystem];
    [controller setTitle:@"Options" forState:UIControlStateNormal];
    controller.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [controller addTarget:self action:@selector(controllerOptions) forControlEvents:UIControlEventTouchUpInside];
    [self addSection:@"System & Input" rows:@[
        [self switchRow:@"CPU optimisation" hint:@"Faster JIT execution with rare accuracy tradeoffs." value:values.cpu_opt output:&_cpuSwitch],
        [self row:@"Virtual controls" hint:@"Opacity, scale, layout, visibility, and physical-pad auto-hide." accessory:controller],
    ]];

    // Performance-overlay toggles live in NSUserDefaults (frontend-only
    // state); they apply immediately without Save.
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [self addSection:@"Performance overlay" rows:@[
        [self row:@"Show FPS" hint:@"Guest frames per second, sampled every second."
            accessory:[self defaultsSwitch:@"vita3k.perf.fps" defaults:defaults]],
        [self row:@"Show RAM usage" hint:@"This app's physical memory footprint."
            accessory:[self defaultsSwitch:@"vita3k.perf.ram" defaults:defaults]],
        [self row:@"Show battery %" hint:@"Approximate device level; iOS exposes it in coarse steps."
            accessory:[self defaultsSwitch:@"vita3k.perf.battery" defaults:defaults]],
    ]];

    UISwitch *titleIds = [[UISwitch alloc] init];
    titleIds.on = show_title_ids();
    [titleIds addTarget:self action:@selector(showTitleIdsChanged:) forControlEvents:UIControlEventValueChanged];
    [self addSection:@"Library" rows:@[
        [self row:@"Show title IDs" hint:@"Show the PCSG… identifier under each game." accessory:titleIds],
    ]];

    UIButton *changelog = [UIButton buttonWithType:UIButtonTypeSystem];
    [changelog setTitle:@"Changelog" forState:UIControlStateNormal];
    changelog.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [changelog addTarget:self action:@selector(showChangelog) forControlEvents:UIControlEventTouchUpInside];
    UIButton *forkLink = [UIButton buttonWithType:UIButtonTypeSystem];
    [forkLink setTitle:@"Vita3K ↗" forState:UIControlStateNormal];
    forkLink.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [forkLink addTarget:self action:@selector(openVita3K) forControlEvents:UIControlEventTouchUpInside];
    [self addSection:@"About" rows:@[
        [self row:@"Version" hint:@"Tsubomi — an iOS PS Vita emulator." accessory:[self valueLabel:tsubomi_app_version()]],
        [self row:@"What's new" hint:@"Changes in this version." accessory:changelog],
        [self row:@"Forked from" hint:@"Tsubomi is built on the Vita3K emulator." accessory:forkLink],
        [self row:@"Developed by" hint:@"twitter / discord" accessory:[self valueLabel:@"@halcyonpalace"]],
        [self row:@"Thanks to" hint:@"" accessory:[self valueLabel:@"Bloom, Craig, Thomasina"]],
    ]];
    return self;
}

- (void)showTitleIdsChanged:(UISwitch *)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.on forKey:@"tsubomi.showTitleIds"];
    reload_library_cells();
}

- (void)openVita3K {
    [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"https://github.com/Vita3K/Vita3K"]
                                    options:@{}
                          completionHandler:nil];
}

- (void)showChangelog {
    present_alert(@"What's new in 0.5.0",
        @"• Renamed to Tsubomi (data now in Documents/Tsubomi).\n"
        @"• iOS audio session fix and movie/audio decode work.\n"
        @"• Import fixes: zip symlink false-reject, NoNpDrm work.bin decrypt, "
        @"failed boots return to the library.\n"
        @"• JIT banner no longer reappears after StikDebug detaches.\n"
        @"• Library: long-press to rename a game, Show title IDs toggle, "
        @"landscape spacing, firmware version badge.\n"
        @"• App icon, About section, and various UI polish.");
}

- (UISwitch *)defaultsSwitch:(NSString *)key defaults:(NSUserDefaults *)defaults {
    UISwitch *control = [[UISwitch alloc] init];
    control.on = [defaults boolForKey:key];
    control.accessibilityIdentifier = key;
    [control addTarget:self action:@selector(perfToggleChanged:) forControlEvents:UIControlEventValueChanged];
    return control;
}

- (void)perfToggleChanged:(UISwitch *)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.on forKey:sender.accessibilityIdentifier];
}

- (UILabel *)valueLabel:(NSString *)value {
    UILabel *label = [[UILabel alloc] init];
    label.text = value;
    label.textColor = UIColor.secondaryLabelColor;
    label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    return label;
}

- (UIView *)switchRow:(NSString *)title hint:(NSString *)hint value:(BOOL)value output:(UISwitch *__strong *)output {
    UISwitch *control = [[UISwitch alloc] init];
    control.on = value;
    *output = control;
    return [self row:title hint:hint accessory:control];
}

- (UIView *)row:(NSString *)title hint:(NSString *)hint accessory:(UIView *)accessory {
    UIStackView *labels = [[UIStackView alloc] init];
    labels.axis = UILayoutConstraintAxisVertical;
    labels.spacing = 3;
    UILabel *name = [[UILabel alloc] init];
    name.text = title;
    name.textColor = UIColor.labelColor;
    name.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    UILabel *detail = [[UILabel alloc] init];
    detail.text = hint;
    detail.textColor = UIColor.secondaryLabelColor;
    detail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    detail.numberOfLines = 0;
    [labels addArrangedSubview:name];
    [labels addArrangedSubview:detail];

    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[labels, accessory]];
    const BOOL wideAccessory = [accessory.accessibilityIdentifier isEqualToString:@"wideAccessory"];
    row.axis = wideAccessory ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    row.alignment = wideAccessory ? UIStackViewAlignmentFill : UIStackViewAlignmentCenter;
    row.spacing = 16;
    row.layoutMargins = UIEdgeInsetsMake(11, 14, 11, 14);
    row.layoutMarginsRelativeArrangement = YES;
    [labels setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [accessory setContentHuggingPriority:wideAccessory ? UILayoutPriorityDefaultLow : UILayoutPriorityRequired
                                forAxis:UILayoutConstraintAxisHorizontal];
    return row;
}

- (void)addSection:(NSString *)title rows:(NSArray<UIView *> *)rows {
    UILabel *label = [[UILabel alloc] init];
    label.text = title.uppercaseString;
    label.textColor = UIColor.systemPinkColor;
    label.font = [UIFont systemFontOfSize:13 weight:UIFontWeightBold];
    UIStackView *content = [[UIStackView alloc] init];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 1;
    [content addArrangedSubview:label];
    for (UIView *row in rows)
        [content addArrangedSubview:row];
    // Plain adaptive material, not interactive glass: live glass refraction on
    // every section made the settings scroll visibly stutter. The ...Dark
    // variant was also wrong in light mode.
    UIVisualEffectView *glass = [[UIVisualEffectView alloc]
        initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterial]];
    glass.layer.cornerRadius = 24;
    glass.clipsToBounds = YES;
    [glass.contentView addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:glass.contentView.leadingAnchor constant:14],
        [content.trailingAnchor constraintEqualToAnchor:glass.contentView.trailingAnchor constant:-14],
        [content.topAnchor constraintEqualToAnchor:glass.contentView.topAnchor constant:14],
        [content.bottomAnchor constraintEqualToAnchor:glass.contentView.bottomAnchor constant:-14],
    ]];
    [self.stack addArrangedSubview:glass];
}

- (void)resolutionChanged:(UISlider *)slider {
    slider.value = roundf(slider.value * 4.0f) / 4.0f;
    [self updateResolutionLabel];
}

- (void)fpsChanged:(UISlider *)slider {
    slider.value = roundf(slider.value / 5.0f) * 5.0f;
    self.fpsValue.text = [NSString stringWithFormat:@"%.0f FPS", slider.value];
}

- (void)updateResolutionLabel {
    self.resolutionValue.text = [NSString stringWithFormat:@"%.2gx", self.resolutionSlider.value];
}

- (void)controllerOptions {
    vita3k_ios_present_controller_options();
}

- (void)close {
    [UIView animateWithDuration:0.22 animations:^{ self.alpha = 0; } completion:^(__unused BOOL finished) {
        [self removeFromSuperview];
    }];
}

- (void)save {
    const int anisotropicValues[] = {1, 2, 4, 8, 16};
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ApplySettings;
    action.settings.resolution_multiplier = self.resolutionSlider.value;
    action.settings.v_sync = self.vsyncSwitch.on;
    action.settings.fps_limit = (int)self.fpsSlider.value;
    [NSUserDefaults.standardUserDefaults setInteger:action.settings.fps_limit forKey:@"tsubomi.fpsLimit"];
    action.settings.cpu_opt = self.cpuSwitch.on;
    action.settings.ngs_enable = self.ngsSwitch.on;
    action.settings.async_pipeline_compilation = self.asyncSwitch.on;
    action.settings.anisotropic_filtering = anisotropicValues[self.anisotropicControl.selectedSegmentIndex];
    queue_action(std::move(action));
    [self close];
}

@end


@interface Vita3KLibraryView : UIView <UICollectionViewDataSource, UICollectionViewDelegateFlowLayout> {
@public
    std::vector<Vita3KIOSGameEntry> _games;
    Vita3KIOSSettings _settings;
    BOOL _jitAvailable;
    BOOL _listMode;
    CGFloat _lastCollectionWidth;
}
@property(nonatomic, strong) UICollectionView *collectionView;
@property(nonatomic, strong) UILabel *emptyLabel;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIView *busyOverlay;
@property(nonatomic, strong) UIVisualEffectView *headerGlass;
@property(nonatomic, strong) CAGradientLayer *backgroundGradient;
@property(nonatomic, strong) UIVisualEffectView *firmwareGlass;
@property(nonatomic, strong) UILabel *firmwareLabel;
@property(nonatomic, strong) UIVisualEffectView *jitBanner;
@property(nonatomic, strong) UILabel *jitBannerLabel;
- (void)updateGames:(const std::vector<Vita3KIOSGameEntry> &)games settings:(const Vita3KIOSSettings &)settings;
- (void)setJitAvailable:(BOOL)available;
- (BOOL)jitAvailable;
- (BOOL)firmwareReadyOrPresentAlert;
@end

@implementation Vita3KLibraryView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    _jitAvailable = YES;
    _listMode = [NSUserDefaults.standardUserDefaults boolForKey:@"tsubomi.libraryListMode"];
    // Enable battery monitoring once (not per perf-overlay tick).
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    self.backgroundColor = UIColor.systemBackgroundColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundGradient = [CAGradientLayer layer];
    self.backgroundGradient.startPoint = CGPointMake(0, 0);
    self.backgroundGradient.endPoint = CGPointMake(1, 1);
    [self.layer insertSublayer:self.backgroundGradient atIndex:0];
    [self updateGradientColors];

    // The library scrolls edge-to-edge underneath the header; this bar blurs
    // whatever passes below the title/controls, exactly like a system
    // navigation bar's scroll-edge appearance. It stays invisible while the
    // content is at rest at the top.
    self.headerGlass = [[UIVisualEffectView alloc]
        initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterial]];
    self.headerGlass.alpha = 0;
    [self addSubview:self.headerGlass];

    UILabel *title = [[UILabel alloc] init];
    title.text = @"Tsubomi";
    title.font = [UIFont systemFontOfSize:32 weight:UIFontWeightBold];
    title.textColor = UIColor.labelColor;
    // Buttons share the title row; on narrow phones the title yields first.
    title.adjustsFontSizeToFitWidth = YES;
    title.minimumScaleFactor = 0.7;
    title.tag = 101;
    [self addSubview:title];

    UIButton *refresh = symbol_button(@"arrow.clockwise", @"Refresh", @"Refresh game library");
    refresh.tag = 102;
    [refresh addTarget:self action:@selector(refresh) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:refresh];
    UIButton *settings = symbol_button(@"gearshape.fill", @"Settings", @"Settings");
    settings.tag = 103;
    [settings addTarget:self action:@selector(settings) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:settings];
    UIButton *importButton = symbol_button(@"plus", @"Add", @"Import game or firmware");
    importButton.tag = 104;
    // Attach the choices directly to the + button so iOS morphs the menu out
    // of the glass control itself instead of sliding an action sheet up from
    // the bottom of the screen.
    __weak Vita3KLibraryView *weakSelf = self;
    UIAction *importGame = [UIAction actionWithTitle:@"Import game (.vpk / .zip / .pkg)"
        image:[UIImage systemImageNamed:@"arrow.down.doc"]
        identifier:nil
        handler:^(__unused UIAction *action) {
            if ([weakSelf firmwareReadyOrPresentAlert])
                present_import_picker(NO);
        }];
    UIAction *importFirmware = [UIAction actionWithTitle:@"Import firmware (.PUP)"
        image:[UIImage systemImageNamed:@"cpu"]
        identifier:nil
        handler:^(__unused UIAction *action) { present_import_picker(YES); }];
    UIAction *importLicense = [UIAction actionWithTitle:@"Import license (work.bin)"
        image:[UIImage systemImageNamed:@"key.fill"]
        identifier:nil
        handler:^(__unused UIAction *action) { present_license_picker(); }];
    importButton.menu = [UIMenu menuWithChildren:@[importGame, importLicense, importFirmware]];
    importButton.showsMenuAsPrimaryAction = YES;
    [self addSubview:importButton];
    UIButton *viewMode = symbol_button(_listMode ? @"square.grid.2x2" : @"list.bullet", @"View", @"Switch library view");
    viewMode.tag = 105;
    [viewMode addTarget:self action:@selector(toggleViewMode:) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:viewMode];

    // Firmware version indicator, wrapped in a small glass capsule.
    self.firmwareGlass = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    self.firmwareGlass.layer.cornerRadius = 11;
    self.firmwareGlass.clipsToBounds = YES;
    self.firmwareGlass.hidden = YES;
    self.firmwareLabel = [[UILabel alloc] init];
    self.firmwareLabel.textColor = UIColor.secondaryLabelColor;
    self.firmwareLabel.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightSemibold];
    self.firmwareLabel.textAlignment = NSTextAlignmentCenter;
    [self.firmwareGlass.contentView addSubview:self.firmwareLabel];
    [self addSubview:self.firmwareGlass];

    // Persistent notice shown when JIT is not enabled; games cannot boot.
    self.jitBanner = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    self.jitBanner.layer.cornerRadius = 16;
    self.jitBanner.clipsToBounds = YES;
    self.jitBanner.hidden = YES;
    self.jitBannerLabel = [[UILabel alloc] init];
    self.jitBannerLabel.text = @"JIT is not ready — open StikDebug, enable JIT, and keep it attached until Tsubomi finishes Preparing JIT.";
    self.jitBannerLabel.textColor = UIColor.labelColor;
    self.jitBannerLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    self.jitBannerLabel.numberOfLines = 0;
    UIImageView *jitIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"exclamationmark.triangle.fill"]];
    jitIcon.tintColor = UIColor.systemYellowColor;
    jitIcon.tag = 201;
    [self.jitBanner.contentView addSubview:jitIcon];
    [self.jitBanner.contentView addSubview:self.jitBannerLabel];
    [self addSubview:self.jitBanner];

    UICollectionViewFlowLayout *layout = [[UICollectionViewFlowLayout alloc] init];
    layout.minimumInteritemSpacing = 14;
    layout.minimumLineSpacing = 18;
    self.collectionView = [[UICollectionView alloc] initWithFrame:CGRectZero collectionViewLayout:layout];
    self.collectionView.backgroundColor = UIColor.clearColor;
    self.collectionView.dataSource = self;
    self.collectionView.delegate = self;
    self.collectionView.alwaysBounceVertical = YES;
    // Full-bleed: the collection view covers the whole screen and the header
    // floats above it; contentInset (set in layoutSubviews) keeps the resting
    // position below the header while scrolled content slides underneath.
    self.collectionView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    [self.collectionView registerClass:Vita3KGameCell.class forCellWithReuseIdentifier:@"game"];
    [self insertSubview:self.collectionView belowSubview:self.headerGlass];

    self.emptyLabel = [[UILabel alloc] init];
    self.emptyLabel.text = @"No games yet\n\nTap + to import a game (.vpk/.zip/.pkg), or copy\nPC's Vita3K data into Documents/Tsubomi/vita";
    self.emptyLabel.textColor = UIColor.secondaryLabelColor;
    self.emptyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    self.emptyLabel.textAlignment = NSTextAlignmentCenter;
    self.emptyLabel.numberOfLines = 0;
    [self addSubview:self.emptyLabel];

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.textColor = UIColor.systemOrangeColor;
    self.statusLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.numberOfLines = 2;
    self.statusLabel.alpha = 0;
    [self addSubview:self.statusLabel];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.backgroundGradient.frame = self.bounds;
    const UIEdgeInsets safe = self.safeAreaInsets;
    UILabel *title = [self viewWithTag:101];
    UIButton *refresh = [self viewWithTag:102];
    UIButton *settings = [self viewWithTag:103];
    UIButton *importButton = [self viewWithTag:104];
    UIButton *viewMode = [self viewWithTag:105];
    const CGFloat usableWidth = CGRectGetWidth(self.bounds) - safe.left - safe.right;
    const BOOL compactHeader = usableWidth < 600;
    // Single navigation-bar-style row: large title on the left, plain glyph
    // controls right-aligned on the same baseline.
    const CGFloat rowY = safe.top + 8;
    const CGFloat rowHeight = 44;
    const CGFloat buttonSize = 40;
    const CGFloat buttonY = rowY + (rowHeight - buttonSize) / 2;
    settings.frame = CGRectMake(CGRectGetWidth(self.bounds) - safe.right - 14 - buttonSize, buttonY, buttonSize, buttonSize);
    refresh.frame = CGRectMake(CGRectGetMinX(settings.frame) - buttonSize - 2, buttonY, buttonSize, buttonSize);
    importButton.frame = CGRectMake(CGRectGetMinX(refresh.frame) - buttonSize - 2, buttonY, buttonSize, buttonSize);
    viewMode.frame = CGRectMake(CGRectGetMinX(importButton.frame) - buttonSize - 2, buttonY, buttonSize, buttonSize);
    title.frame = CGRectMake(safe.left + 20, rowY,
        MAX(80, CGRectGetMinX(viewMode.frame) - safe.left - 30), rowHeight);

    [self.firmwareLabel sizeToFit];
    const CGFloat firmwareWidth = MIN(CGRectGetWidth(self.firmwareLabel.bounds) + 22, 200);
    const CGFloat firmwareHeight = 22;
    self.firmwareGlass.frame = CGRectMake(CGRectGetMaxX(settings.frame) - firmwareWidth,
        CGRectGetMaxY(settings.frame) + 4, firmwareWidth, firmwareHeight);
    self.firmwareLabel.frame = self.firmwareGlass.bounds;

    CGFloat headerBottom = MAX(CGRectGetMaxY(title.frame), CGRectGetMaxY(settings.frame));
    if (!self.firmwareGlass.hidden)
        headerBottom = MAX(headerBottom, CGRectGetMaxY(self.firmwareGlass.frame));
    // The status label is a transient toast floating over the content edge;
    // it must not reserve permanent header height.
    self.statusLabel.frame = CGRectMake(safe.left + 20, headerBottom + 5,
        MAX(1, usableWidth - 40), 38);
    CGFloat contentTop = headerBottom + 14;
    if (!self.jitBanner.hidden) {
        const CGFloat bannerX = safe.left + 18;
        const CGFloat bannerWidth = CGRectGetWidth(self.bounds) - safe.left - safe.right - 36;
        const CGFloat labelX = 48;
        const CGFloat labelWidth = MAX(1, bannerWidth - labelX - 16);
        const CGSize textSize = [self.jitBannerLabel sizeThatFits:CGSizeMake(labelWidth, CGFLOAT_MAX)];
        const CGFloat bannerHeight = MAX(54, textSize.height + 24);
        self.jitBanner.frame = CGRectMake(bannerX, contentTop, bannerWidth, bannerHeight);
        UIImageView *jitIcon = [self.jitBanner.contentView viewWithTag:201];
        jitIcon.frame = CGRectMake(16, (bannerHeight - 24) / 2, 24, 24);
        self.jitBannerLabel.frame = CGRectMake(labelX, 12, labelWidth, bannerHeight - 24);
        contentTop = CGRectGetMaxY(self.jitBanner.frame) + 10;
    }

    const CGFloat collectionInset = compactHeader ? 10 : 18;
    // The header bar blurs everything that scrolls beneath the controls; it
    // reaches just under the firmware badge (the status label floats over
    // content when it appears).
    self.headerGlass.frame = CGRectMake(0, 0, CGRectGetWidth(self.bounds), headerBottom + 8);
    self.collectionView.frame = self.bounds;
    self.collectionView.contentInset = UIEdgeInsetsMake(contentTop, safe.left + collectionInset,
        safe.bottom + 12, safe.right + collectionInset);
    self.collectionView.verticalScrollIndicatorInsets = UIEdgeInsetsMake(headerBottom + 8, 0, safe.bottom, 0);
    self.emptyLabel.frame = CGRectMake(safe.left + 40, contentTop + 40,
        MAX(1, usableWidth - 80),
        MAX(1, CGRectGetHeight(self.bounds) - contentTop - safe.bottom - 80));
    const CGFloat width = CGRectGetWidth(self.collectionView.bounds)
        - self.collectionView.contentInset.left - self.collectionView.contentInset.right;
    if (fabs(width - _lastCollectionWidth) > 0.5) {
        _lastCollectionWidth = width;
        [self.collectionView.collectionViewLayout invalidateLayout];
    }
    [self updateHeaderGlassVisibility];
}

// Fade the header material in only when content is actually behind it, the
// way a navigation bar transitions from its scroll-edge appearance.
- (void)updateHeaderGlassVisibility {
    const CGFloat scrolled = self.collectionView.contentOffset.y + self.collectionView.contentInset.top;
    const CGFloat alpha = MAX(0.0, MIN(1.0, scrolled / 24.0));
    if (fabs(self.headerGlass.alpha - alpha) > 0.01)
        self.headerGlass.alpha = alpha;
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
    if (scrollView == self.collectionView)
        [self updateHeaderGlassVisibility];
}

- (void)toggleViewMode:(UIButton *)sender {
    _listMode = !_listMode;
    [NSUserDefaults.standardUserDefaults setBool:_listMode forKey:@"tsubomi.libraryListMode"];
    [sender setImage:[UIImage systemImageNamed:_listMode ? @"square.grid.2x2" : @"list.bullet"]
            forState:UIControlStateNormal];
    [self.collectionView.collectionViewLayout invalidateLayout];
    [self.collectionView reloadData];
}

// The gradient uses CGColors, which do not auto-resolve to the current trait
// collection; refresh them whenever light/dark mode changes.
- (void)traitCollectionDidChange:(UITraitCollection *)previous {
    [super traitCollectionDidChange:previous];
    if ([self.traitCollection hasDifferentColorAppearanceComparedToTraitCollection:previous])
        [self updateGradientColors];
}

- (void)updateGradientColors {
    const BOOL dark = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark;
    // Dark: pure black. Light: near-white with a faint pink/cyan wash so the
    // brand feel survives without a heavy tint.
    if (dark) {
        self.backgroundGradient.colors = @[
            (id)UIColor.blackColor.CGColor,
            (id)UIColor.blackColor.CGColor,
            (id)UIColor.blackColor.CGColor,
        ];
    } else {
        self.backgroundGradient.colors = @[
            (id)[UIColor colorWithRed:0.99 green:0.97 blue:0.99 alpha:1].CGColor,
            (id)[UIColor colorWithRed:0.98 green:0.95 blue:0.99 alpha:1].CGColor,
            (id)[UIColor colorWithRed:0.95 green:0.99 blue:1.00 alpha:1].CGColor,
        ];
    }
}

- (void)updateGames:(const std::vector<Vita3KIOSGameEntry> &)games settings:(const Vita3KIOSSettings &)settings {
    _games = games;
    _settings = settings;
    self.emptyLabel.hidden = !_games.empty();
    self.collectionView.hidden = _games.empty();
    NSString *firmware = [NSString stringWithUTF8String:settings.firmware_version.c_str()] ?: @"";
    self.firmwareLabel.text = settings.firmware_ready
        ? firmware
        : [NSString stringWithFormat:@"%@ - setup incomplete", firmware];
    self.firmwareGlass.hidden = (firmware.length == 0);
    self.emptyLabel.text = settings.firmware_ready
        ? @"No games yet\n\nTap + to import a game (.vpk/.zip/.pkg), or copy\nPC's Vita3K data into Documents/Tsubomi/vita"
        : @"Complete firmware setup first\n\nUse + to install FONTPKG.PUP, PREINSTALL.PUP,\nand PSVUPDAT.PUP before importing games.";
    [self setNeedsLayout];
    [self.collectionView reloadData];
}

- (BOOL)firmwareReadyOrPresentAlert {
    if (_settings.firmware_ready)
        return YES;
    NSString *missing = [NSString stringWithUTF8String:_settings.missing_firmware.c_str()] ?: @"required firmware";
    present_alert(@"Complete firmware setup",
        [NSString stringWithFormat:@"Install all three firmware packages before importing or playing games.\n\nMissing: %@\n\nUse + > Import firmware (.PUP).", missing]);
    return NO;
}

- (BOOL)jitAvailable {
    return _jitAvailable;
}

- (void)setJitAvailable:(BOOL)available {
    if (_jitAvailable == available && self.jitBanner.hidden == available)
        return; // No visible change (banner hidden iff JIT available).
    _jitAvailable = available;
    self.jitBanner.hidden = available;
    [self setNeedsLayout];
}

- (NSInteger)collectionView:(UICollectionView *)collectionView numberOfItemsInSection:(NSInteger)section {
    (void)collectionView;
    (void)section;
    return static_cast<NSInteger>(_games.size());
}

- (__kindof UICollectionViewCell *)collectionView:(UICollectionView *)collectionView cellForItemAtIndexPath:(NSIndexPath *)indexPath {
    Vita3KGameCell *cell = [collectionView dequeueReusableCellWithReuseIdentifier:@"game" forIndexPath:indexPath];
    const auto &game = _games.at(static_cast<std::size_t>(indexPath.item));
    NSString *identifier = [NSString stringWithUTF8String:game.title_id.c_str()] ?: @"Unknown title ID";
    NSString *title = [NSString stringWithUTF8String:game.title.c_str()];
    if (!title) {
        LOG_ERROR("iOS library title is invalid UTF-8: title_id={} bytes={}", game.title_id, hex_bytes(game.title));
        title = [NSString stringWithFormat:@"Unknown title (%@)", identifier];
    }
    NSString *iconPath = [NSString stringWithUTF8String:game.icon_path.c_str()];
    [cell configureTitle:display_title(identifier, title) identifier:identifier metadata:game_metadata(game)
                  iconPath:iconPath listMode:_listMode];
    cell.alpha = _settings.firmware_ready ? 1.0 : 0.55;
    return cell;
}

- (void)promptRename:(NSString *)identifier original:(NSString *)original {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename game"
                                                                  message:original
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.text = display_title(identifier, original);
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
        field.autocapitalizationType = UITextAutocapitalizationTypeWords;
    }];
    __weak Vita3KLibraryView *weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *action) {
            NSString *entered = [alert.textFields.firstObject.text
                stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
            // An empty name clears the override (restores the SFO title).
            if (entered.length && ![entered isEqualToString:original])
                [defaults setObject:entered forKey:title_override_key(identifier)];
            else
                [defaults removeObjectForKey:title_override_key(identifier)];
            [weakSelf.collectionView reloadData];
        }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [root presentViewController:alert animated:YES completion:nil];
}

- (UIContextMenuConfiguration *)collectionView:(UICollectionView *)collectionView
    contextMenuConfigurationForItemAtIndexPath:(NSIndexPath *)indexPath
                                         point:(CGPoint)point {
    (void)collectionView;
    (void)point;
    if (indexPath.item >= static_cast<NSInteger>(_games.size()))
        return nil;
    const auto &game = _games.at(static_cast<std::size_t>(indexPath.item));
    NSString *identifier = [NSString stringWithUTF8String:game.title_id.c_str()] ?: @"";
    NSString *original = [NSString stringWithUTF8String:game.title.c_str()] ?: identifier;
    NSString *trophyId = [NSString stringWithUTF8String:game.trophy_id.c_str()] ?: @"";
    __weak Vita3KLibraryView *weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil
        previewProvider:nil
         actionProvider:^UIMenu *(__unused NSArray<UIMenuElement *> *suggested) {
            UIAction *rename = [UIAction actionWithTitle:@"Rename title"
                image:[UIImage systemImageNamed:@"pencil"]
                identifier:nil
                handler:^(__unused UIAction *action) { [weakSelf promptRename:identifier original:original]; }];
            NSString *overrideName = [NSUserDefaults.standardUserDefaults stringForKey:title_override_key(identifier)];
            UIAction *importSave = [UIAction actionWithTitle:@"Import save"
                image:[UIImage systemImageNamed:@"square.and.arrow.down"]
                identifier:nil handler:^(__unused UIAction *action) { present_save_picker(identifier); }];
            UIAction *exportSave = [UIAction actionWithTitle:@"Export save"
                image:[UIImage systemImageNamed:@"square.and.arrow.up"]
                identifier:nil handler:^(__unused UIAction *action) {
                    [weakSelf showBusyOverlay:@"Exporting save…" blockInteraction:YES];
                    Vita3KIOSFrontendAction request;
                    request.kind = Vita3KIOSFrontendActionKind::ExportSave;
                    request.title_id = identifier.UTF8String;
                    queue_action(std::move(request));
                }];
            UIAction *trophies = [UIAction actionWithTitle:@"View trophies"
                image:[UIImage systemImageNamed:@"trophy.fill"]
                identifier:nil handler:^(__unused UIAction *action) {
                    Vita3KIOSFrontendAction request;
                    request.kind = Vita3KIOSFrontendActionKind::ShowTrophies;
                    request.title_id = original.UTF8String;
                    request.app_path = identifier.UTF8String;
                    request.trophy_id = trophyId.UTF8String;
                    queue_action(std::move(request));
                }];
            NSMutableArray<UIMenuElement *> *children = [NSMutableArray arrayWithObjects:
                importSave, exportSave, rename, trophies, nil];
            if (overrideName.length) {
                UIAction *reset = [UIAction actionWithTitle:@"Reset name"
                    image:[UIImage systemImageNamed:@"arrow.uturn.backward"]
                    identifier:nil
                    handler:^(__unused UIAction *action) {
                        [NSUserDefaults.standardUserDefaults removeObjectForKey:title_override_key(identifier)];
                        [weakSelf.collectionView reloadData];
                    }];
                reset.attributes = UIMenuElementAttributesDestructive;
                [children addObject:reset];
            }
            return [UIMenu menuWithTitle:original children:children];
        }];
}

- (CGSize)collectionView:(UICollectionView *)collectionView layout:(UICollectionViewLayout *)layout sizeForItemAtIndexPath:(NSIndexPath *)indexPath {
    (void)layout;
    (void)indexPath;
    const CGFloat width = CGRectGetWidth(collectionView.bounds)
        - collectionView.contentInset.left - collectionView.contentInset.right;
    if (_listMode)
        return CGSizeMake(floor(width), 82);
    const CGFloat spacing = 14;
    // Choose the column count from a larger target cell width (~220pt) rather than a
    // couple of fixed width thresholds. Phone landscape (~750-800pt of grid)
    // then packs 4 sensible cells instead of 3 ballooned ones, while portrait
    // still lands on 2 columns.
    const CGFloat targetItemWidth = 220;
    const NSInteger columns = MAX(2, static_cast<NSInteger>(floor((width + spacing) / (targetItemWidth + spacing))));
    const CGFloat itemWidth = floor((width - (columns - 1) * spacing) / columns);
    return CGSizeMake(itemWidth, itemWidth + (show_title_ids() ? 112 : 95));
}

- (void)collectionView:(UICollectionView *)collectionView didSelectItemAtIndexPath:(NSIndexPath *)indexPath {
    (void)collectionView;
    if (![self firmwareReadyOrPresentAlert]) {
        [collectionView deselectItemAtIndexPath:indexPath animated:YES];
        return;
    }
    if (!_jitAvailable) {
        // Guest execution needs JIT; refuse the boot and explain why instead of
        // letting the launch path hit the missing-debugger crash boundary.
        [collectionView deselectItemAtIndexPath:indexPath animated:YES];
        present_alert(@"JIT required",
            @"Open StikDebug, enable JIT, and keep it attached until Tsubomi finishes Preparing JIT.");
        return;
    }
    UIImpactFeedbackGenerator *feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
    [feedback impactOccurred];
    const auto &game = _games.at(static_cast<std::size_t>(indexPath.item));
    [self showBootingOverlay:[NSString stringWithUTF8String:game.title.c_str()] ?: @"game"];
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::Launch;
    action.app_path = game.app_path;
    queue_action(std::move(action));
}

- (void)showBootingOverlay:(NSString *)title {
    [self showBusyOverlay:[NSString stringWithFormat:@"Preparing JIT and booting %@…\nKeep StikDebug attached", title]
           blockInteraction:YES];
}

- (void)hideBusyOverlay {
    self.userInteractionEnabled = YES;
    [self.busyOverlay removeFromSuperview];
    self.busyOverlay = nil;
}

- (void)showBusyOverlay:(NSString *)text blockInteraction:(BOOL)block {
    [self hideBusyOverlay];
    // Block further taps and make the work visibly in progress; for boots the
    // whole library view is removed once the emulator takes over the screen.
    self.userInteractionEnabled = !block;
    UIView *dim = [[UIView alloc] initWithFrame:self.bounds];
    dim.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    dim.backgroundColor = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark
            ? [UIColor colorWithWhite:0 alpha:0.55]
            : [UIColor colorWithWhite:1 alpha:0.58];
    }];
    dim.alpha = 0;

    UIVisualEffectView *panel = [[UIVisualEffectView alloc]
        initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterial]];
    panel.frame = CGRectMake(0, 0, 260, 130);
    panel.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    panel.autoresizingMask = UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin
        | UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    panel.layer.cornerRadius = 26;
    panel.clipsToBounds = YES;

    UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
    spinner.color = UIColor.labelColor;
    spinner.center = CGPointMake(130, 48);
    [spinner startAnimating];
    [panel.contentView addSubview:spinner];

    UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(14, 82, 232, 36)];
    label.text = text;
    label.textColor = UIColor.labelColor;
    label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 2;
    label.adjustsFontSizeToFitWidth = YES;
    [panel.contentView addSubview:label];

    [dim addSubview:panel];
    [self addSubview:dim];
    self.busyOverlay = dim;
    [UIView animateWithDuration:0.2 animations:^{ dim.alpha = 1; }];
}

- (void)refresh {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::Refresh;
    queue_action(std::move(action));
}

- (void)settings {
    Vita3KSettingsView *settings = [[Vita3KSettingsView alloc] initWithFrame:self.bounds values:_settings];
    settings.alpha = 0;
    [self addSubview:settings];
    [UIView animateWithDuration:0.22 animations:^{ settings.alpha = 1; }];
}

- (void)reportRestartRequired:(NSArray<NSString *> *)settings {
    self.statusLabel.text = settings.count
        ? [NSString stringWithFormat:@"Saved · restart required: %@", [settings componentsJoinedByString:@", "]]
        : @"Settings saved and applied";
    self.statusLabel.alpha = 1;
    [UIView animateWithDuration:0.3 delay:4 options:0 animations:^{ self.statusLabel.alpha = 0; } completion:nil];
}

@end


static Vita3KLibraryView *g_library = nil;
// Last-known JIT availability, applied whenever the library is (re)shown so the
// banner is correct even across library rebuilds between game sessions.
static BOOL g_jit_available = YES;

namespace {
void reload_library_cells() {
    [g_library.collectionView.collectionViewLayout invalidateLayout];
    [g_library.collectionView reloadData];
}
} // namespace

// Presents the Files picker and hands the copied file to the emulator loop.
@interface Vita3KImportPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) Vita3KIOSFrontendActionKind kind;
@property(nonatomic, copy) NSString *titleId;
@end

@implementation Vita3KImportPicker

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *url = urls.firstObject;
    if (!url)
        return;
    const BOOL scoped = [url startAccessingSecurityScopedResource];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *importDir = [[documents stringByAppendingPathComponent:@"Tsubomi"] stringByAppendingPathComponent:@"import"];
    [NSFileManager.defaultManager createDirectoryAtPath:importDir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *destination = [importDir stringByAppendingPathComponent:url.lastPathComponent];
    [NSFileManager.defaultManager removeItemAtPath:destination error:nil];
    NSError *error = nil;
    const BOOL copied = [NSFileManager.defaultManager copyItemAtURL:url
                                                              toURL:[NSURL fileURLWithPath:destination]
                                                              error:&error];
    if (scoped)
        [url stopAccessingSecurityScopedResource];
    if (!copied) {
        // A common cause is an iCloud-only file whose contents were never
        // downloaded, or insufficient free space for the working copy. Surface
        // the underlying reason instead of a bare "could not copy".
        const char *reason = error.localizedDescription.UTF8String ?: "unknown error";
        LOG_ERROR("iOS import copy failed for '{}': {}",
            url.lastPathComponent.UTF8String ?: "?", reason);
        vita3k_ios_report_import_result(
            std::string("Could not read the selected file: ") + reason
                + ". If it is stored in iCloud, download it in Files first.",
            false);
        return;
    }
    NSString *busy = @"Installing game…";
    if (self.kind == Vita3KIOSFrontendActionKind::ImportFirmware)
        busy = @"Installing firmware…";
    else if (self.kind == Vita3KIOSFrontendActionKind::ImportLicense)
        busy = @"Installing license…";
    else if (self.kind == Vita3KIOSFrontendActionKind::ImportSave)
        busy = @"Importing save…";
    [g_library showBusyOverlay:busy blockInteraction:YES];
    Vita3KIOSFrontendAction action;
    action.kind = self.kind;
    action.app_path = destination.UTF8String;
    action.title_id = self.titleId.UTF8String ?: "";
    queue_action(std::move(action));
}

@end

static Vita3KImportPicker *g_import_picker = nil;

namespace {

// Presents the Files picker for a game/firmware/license import. Attached to the
// + button's UIMenu (games/firmware) and the post-install license prompt.
void present_import_picker(BOOL firmware) {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    if (!g_import_picker)
        g_import_picker = [[Vita3KImportPicker alloc] init];
    g_import_picker.kind = firmware ? Vita3KIOSFrontendActionKind::ImportFirmware
                                    : Vita3KIOSFrontendActionKind::ImportGame;
    g_import_picker.titleId = nil;
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    if (firmware) {
        UTType *pup = [UTType typeWithFilenameExtension:@"pup"];
        if (pup)
            [types addObject:pup];
    } else {
        [types addObject:UTTypeZIP];
        UTType *vpk = [UTType typeWithFilenameExtension:@"vpk"];
        if (vpk)
            [types addObject:vpk];
        UTType *pkg = [UTType typeWithFilenameExtension:@"pkg"];
        if (pkg)
            [types addObject:pkg];
    }
    [types addObject:UTTypeData];
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types];
    picker.delegate = g_import_picker;
    picker.allowsMultipleSelection = NO;
    [root presentViewController:picker animated:YES completion:nil];
}

void present_license_picker() {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    if (!g_import_picker)
        g_import_picker = [[Vita3KImportPicker alloc] init];
    g_import_picker.kind = Vita3KIOSFrontendActionKind::ImportLicense;
    g_import_picker.titleId = nil;
    // A work.bin has no standard UTType; accept any file.
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData]];
    picker.delegate = g_import_picker;
    picker.allowsMultipleSelection = NO;
    [root presentViewController:picker animated:YES completion:nil];
}

void present_save_picker(NSString *titleId) {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    if (!g_import_picker)
        g_import_picker = [[Vita3KImportPicker alloc] init];
    g_import_picker.kind = Vita3KIOSFrontendActionKind::ImportSave;
    g_import_picker.titleId = titleId;
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeZIP, UTTypeData]];
    picker.delegate = g_import_picker;
    picker.allowsMultipleSelection = NO;
    [root presentViewController:picker animated:YES completion:nil];
}

} // namespace

void vita3k_ios_show_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings) {
    const std::vector<Vita3KIOSGameEntry> gamesCopy = games;
    const Vita3KIOSSettings settingsCopy = settings;
    perform_on_main(^{
        UIWindow *window = active_window();
        if (!window)
            return;
        // Host the library under the root view controller's view, not directly
        // on the window. UIButton context menus (the + import menu) need a view
        // controller in the responder chain to present from; a view parented
        // straight to the window has none, so taps did nothing.
        UIView *host = window.rootViewController.view ?: window;
        if (!g_library) {
            g_library = [[Vita3KLibraryView alloc] initWithFrame:host.bounds];
            [host addSubview:g_library];
        } else if (g_library.superview != host) {
            [host addSubview:g_library];
        }
        [g_library updateGames:gamesCopy settings:settingsCopy];
        [g_library setJitAvailable:g_jit_available];
        [g_library.superview bringSubviewToFront:g_library];
    });
}

void vita3k_ios_update_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings) {
    const std::vector<Vita3KIOSGameEntry> gamesCopy = games;
    const Vita3KIOSSettings settingsCopy = settings;
    perform_on_main(^{
        [g_library updateGames:gamesCopy settings:settingsCopy];
    });
}

void vita3k_ios_hide_library() {
    perform_on_main(^{
        [g_library removeFromSuperview];
        g_library = nil;
    });
}

std::optional<Vita3KIOSFrontendAction> vita3k_ios_take_frontend_action() {
    const std::lock_guard lock(g_action_mutex);
    auto action = std::move(g_pending_action);
    g_pending_action.reset();
    return action;
}

@interface Vita3KTrophyViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property(nonatomic, copy) NSString *collectionTitle;
@property(nonatomic, copy) NSString *progressText;
@property(nonatomic, strong) NSArray<NSDictionary *> *rows;
@property(nonatomic, strong) UITableView *tableView;
@end

@implementation Vita3KTrophyViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.title = self.collectionTitle;
    self.navigationItem.prompt = self.progressText;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(close)];
    self.tableView = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleInsetGrouped];
    self.tableView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    self.tableView.rowHeight = 88;
    [self.view addSubview:self.tableView];
}
- (void)close { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    // When opened from the in-game menu, closing the sheet returns there.
    vita3k_ios_submenu_dismissed();
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section; return self.rows.count;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *identifier = @"trophy";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:identifier];
    if (!cell)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:identifier];
    NSDictionary *row = self.rows[indexPath.row];
    cell.textLabel.text = row[@"name"];
    cell.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    cell.detailTextLabel.text = row[@"detail"];
    cell.detailTextLabel.numberOfLines = 3;
    NSString *path = row[@"icon"];
    UIImage *image = path.length ? [UIImage imageWithContentsOfFile:path] : nil;
    cell.imageView.image = image ?: [UIImage systemImageNamed:[row[@"earned"] boolValue] ? @"trophy.fill" : @"lock.fill"];
    cell.imageView.tintColor = [row[@"earned"] boolValue] ? UIColor.systemYellowColor : UIColor.tertiaryLabelColor;
    cell.accessoryView = nil;
    return cell;
}
@end

int vita3k_ios_load_fps_limit() {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if (![defaults objectForKey:@"tsubomi.fpsLimit"])
        return 60;
    return (int)std::clamp<NSInteger>([defaults integerForKey:@"tsubomi.fpsLimit"], 15, 60);
}

void vita3k_ios_present_trophies(const Vita3KIOSTrophyCollection &collection) {
    const Vita3KIOSTrophyCollection copy = collection;
    perform_on_main(^{
        NSMutableArray<NSDictionary *> *rows = [NSMutableArray arrayWithCapacity:copy.trophies.size()];
        NSDateFormatter *dateFormatter = [[NSDateFormatter alloc] init];
        dateFormatter.dateStyle = NSDateFormatterMediumStyle;
        dateFormatter.timeStyle = NSDateFormatterShortStyle;
        for (const auto &trophy : copy.trophies) {
            NSString *name = [NSString stringWithUTF8String:trophy.name.c_str()] ?: @"Trophy";
            NSString *detail = [NSString stringWithUTF8String:trophy.detail.c_str()] ?: @"";
            if (trophy.hidden && !trophy.earned) {
                name = @"Hidden trophy";
                detail = @"Unlock this trophy to reveal its details.";
            }
            NSString *grade = trophy.grade == 1 ? @"Platinum" : trophy.grade == 2 ? @"Gold"
                : trophy.grade == 3 ? @"Silver" : trophy.grade == 4 ? @"Bronze" : @"Trophy";
            NSString *state = @"Locked";
            if (trophy.earned && trophy.timestamp > 0)
                state = [NSString stringWithFormat:@"Unlocked %@", [dateFormatter stringFromDate:
                    [NSDate dateWithTimeIntervalSince1970:trophy.timestamp]]];
            NSString *combined = detail.length
                ? [NSString stringWithFormat:@"%@ · %@\n%@", grade, state, detail]
                : [NSString stringWithFormat:@"%@ · %@", grade, state];
            [rows addObject:@{
                @"name": name,
                @"detail": combined,
                @"icon": [NSString stringWithUTF8String:trophy.icon_path.c_str()] ?: @"",
                @"earned": @(trophy.earned),
            }];
        }
        Vita3KTrophyViewController *controller = [[Vita3KTrophyViewController alloc] init];
        controller.collectionTitle = [NSString stringWithUTF8String:copy.title.c_str()] ?: @"Trophies";
        controller.progressText = copy.total > 0
            ? [NSString stringWithFormat:@"%d of %d unlocked", copy.unlocked, copy.total]
            : @"No trophy data is installed for this title yet.";
        controller.rows = rows;
        UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:controller];
        navigation.modalPresentationStyle = UIModalPresentationPageSheet;
        [active_window().rootViewController presentViewController:navigation animated:YES completion:nil];
    });
}

void vita3k_ios_share_file(const std::string &path) {
    NSString *filePath = [NSString stringWithUTF8String:path.c_str()];
    perform_on_main(^{
        NSURL *url = [NSURL fileURLWithPath:filePath];
        UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
        UIViewController *root = active_window().rootViewController;
        share.popoverPresentationController.sourceView = root.view;
        share.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(root.view.bounds),
            CGRectGetMidY(root.view.bounds), 1, 1);
        [root presentViewController:share animated:YES completion:nil];
    });
}

void vita3k_ios_request_current_trophies() {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ShowTrophies;
    queue_action(std::move(action));
}

static UIVisualEffectView *g_perf_hud = nil;
static UILabel *g_perf_label = nil;

void vita3k_ios_update_perf_overlay(const float guest_fps) {
    perform_on_main(^{
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        const BOOL show_fps = [defaults boolForKey:@"vita3k.perf.fps"];
        const BOOL show_ram = [defaults boolForKey:@"vita3k.perf.ram"];
        const BOOL show_battery = [defaults boolForKey:@"vita3k.perf.battery"];
        if ([defaults boolForKey:@"vita3k.perf.hidden"] || (!show_fps && !show_ram && !show_battery)) {
            g_perf_hud.hidden = YES;
            return;
        }

        UIWindow *window = active_window();
        if (!window)
            return;
        if (!g_perf_hud) {
            // Non-interactive glass: the HUD repaints every second, so live
            // refraction would be wasted cost.
            g_perf_hud = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
            g_perf_hud.layer.cornerRadius = 12;
            g_perf_hud.clipsToBounds = YES;
            g_perf_hud.userInteractionEnabled = NO;
            g_perf_label = [[UILabel alloc] init];
            g_perf_label.textColor = UIColor.labelColor;
            g_perf_label.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightSemibold];
            [g_perf_hud.contentView addSubview:g_perf_label];
        }
        if (g_perf_hud.superview != window)
            [window addSubview:g_perf_hud];
        g_perf_hud.hidden = NO;
        [window bringSubviewToFront:g_perf_hud];

        NSMutableArray<NSString *> *parts = [NSMutableArray array];
        if (show_fps)
            [parts addObject:[NSString stringWithFormat:@"%.0f FPS", guest_fps]];
        if (show_ram) {
            task_vm_info_data_t vm_info{};
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO,
                    reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS)
                [parts addObject:[NSString stringWithFormat:@"%.0f MB", vm_info.phys_footprint / (1024.0 * 1024.0)]];
        }
        if (show_battery) {
            // batteryMonitoringEnabled is set once at library init, not here.
            // iOS still reports batteryLevel in 5% steps; that granularity is an
            // OS limitation (no public finer API — IOKit is private).
            const float level = UIDevice.currentDevice.batteryLevel;
            if (level >= 0)
                [parts addObject:[NSString stringWithFormat:@"≈%.0f%%", level * 100.0f]];
        }
        g_perf_label.text = [parts componentsJoinedByString:@"  ·  "];
        [g_perf_label sizeToFit];
        const CGFloat width = CGRectGetWidth(g_perf_label.bounds) + 20;
        const UIEdgeInsets safe = window.safeAreaInsets;
        g_perf_hud.frame = CGRectMake(safe.left + 10, safe.top + 6, width, 24);
        g_perf_label.frame = CGRectMake(10, 3, width - 20, 18);
    });
}

void vita3k_ios_hide_perf_overlay() {
    perform_on_main(^{
        [g_perf_hud removeFromSuperview];
        g_perf_hud = nil;
        g_perf_label = nil;
    });
}

void vita3k_ios_report_import_result(const std::string &message, const bool success) {
    NSString *text = [NSString stringWithUTF8String:message.c_str()] ?: @"Import finished";
    perform_on_main(^{
        [g_library hideBusyOverlay];
        if (success) {
            g_library.statusLabel.text = text;
            g_library.statusLabel.alpha = 1;
            [UIView animateWithDuration:0.3 delay:6 options:0 animations:^{ g_library.statusLabel.alpha = 0; } completion:nil];
            if ([text localizedCaseInsensitiveContainsString:@"license"])
                present_alert(@"License installed", text);
        } else {
            // Keep the precise installer detail on screen until dismissed.
            NSString *title = [text localizedCaseInsensitiveContainsString:@"save"]
                ? @"Save transfer failed"
                : @"Import failed";
            present_alert(title, text);
        }
    });
}

void vita3k_ios_show_boot_error(const std::string &message) {
    NSString *text = [NSString stringWithUTF8String:message.c_str()] ?: @"The game could not start.";
    perform_on_main(^{
        [g_library hideBusyOverlay];
        present_alert(@"Couldn’t start game", text);
    });
}

void vita3k_ios_set_jit_available(const bool available) {
    perform_on_main(^{
        g_jit_available = available;
        [g_library setJitAvailable:available];
    });
}

void vita3k_ios_prompt_license_import(const std::string &title_id) {
    NSString *identifier = [NSString stringWithUTF8String:title_id.c_str()] ?: @"this title";
    perform_on_main(^{
        UIViewController *root = active_window().rootViewController;
        if (!root)
            return;
        UIAlertController *prompt = [UIAlertController
            alertControllerWithTitle:@"License required?"
                             message:[NSString stringWithFormat:@"%@ has no NoNpDrm license installed. If this is a NoNpDrm dump, import its work.bin now.", identifier]
                      preferredStyle:UIAlertControllerStyleAlert];
        [prompt addAction:[UIAlertAction actionWithTitle:@"Import work.bin"
                                                   style:UIAlertActionStyleDefault
                                                 handler:^(__unused UIAlertAction *action) { present_license_picker(); }]];
        [prompt addAction:[UIAlertAction actionWithTitle:@"Not now" style:UIAlertActionStyleCancel handler:nil]];
        [root presentViewController:prompt animated:YES completion:nil];
    });
}

void vita3k_ios_configure_audio_session() {
    NSError *error = nil;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    // Playback so audio keeps running with the mute switch on and the audio
    // unit is actually scheduled; MixWithOthers keeps us polite.
    if (![session setCategory:AVAudioSessionCategoryPlayback
                         mode:AVAudioSessionModeDefault
                      options:AVAudioSessionCategoryOptionMixWithOthers
                        error:&error]) {
        LOG_ERROR("iOS audio session setCategory failed: {}",
            error.localizedDescription.UTF8String ?: "unknown");
        error = nil;
    }
    if (![session setPreferredSampleRate:48000 error:&error]) {
        LOG_WARN("iOS audio session preferred sample rate failed: {}",
            error.localizedDescription.UTF8String ?: "unknown");
        error = nil;
    }
    constexpr NSTimeInterval preferredBufferDuration = 1024.0 / 48000.0;
    if (![session setPreferredIOBufferDuration:preferredBufferDuration error:&error]) {
        LOG_WARN("iOS audio session preferred buffer duration failed: {}",
            error.localizedDescription.UTF8String ?: "unknown");
        error = nil;
    }
    if (![session setActive:YES error:&error]) {
        LOG_ERROR("iOS audio session activation failed: {}",
            error.localizedDescription.UTF8String ?: "unknown");
    } else {
        LOG_INFO("iOS audio session active (Playback), sample rate {} Hz, I/O buffer {:.2f} ms",
            session.sampleRate, session.IOBufferDuration * 1000.0);
    }
}

void vita3k_ios_pump_runloop(const double seconds) {
    // On the main thread (where SDL runs main() on iOS) the run loop owns the
    // UIKit event/timer sources, so running it for `seconds` keeps scrolling,
    // sliders, and glass animations smooth instead of the blind SDL_Delay that
    // starved them. returnAfterSourceHandled:false makes it block the full
    // interval rather than spinning after each event. Off the main thread the
    // run loop has no sources and would return instantly, so sleep instead to
    // avoid a busy loop.
    if (NSThread.isMainThread)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    else
        [NSThread sleepForTimeInterval:seconds];
}

void vita3k_ios_report_settings_result(const std::vector<std::string> &restart_required) {
    NSMutableArray<NSString *> *labels = [NSMutableArray arrayWithCapacity:restart_required.size()];
    for (const auto &label : restart_required)
        [labels addObject:[NSString stringWithUTF8String:label.c_str()]];
    perform_on_main(^{
        [g_library reportRestartRequired:labels];
    });
}
