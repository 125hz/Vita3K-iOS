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
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <mach/mach.h>
#undef Ptr

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

UIVisualEffect *glass_effect(const BOOL interactive = YES) {
    if (@available(iOS 26.0, *)) {
        UIGlassEffect *effect = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
        effect.interactive = interactive;
        effect.tintColor = [UIColor colorWithWhite:0.04 alpha:0.12];
        return effect;
    }
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
}

UIButton *symbol_button(NSString *symbol, NSString *fallback, NSString *accessibility) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    // Size the button BEFORE adding the autoresizing glass child: adding a
    // 44pt child to a zero-sized parent made autoresizing inflate the glass
    // past the button (the oversized top-right blobs on the home screen).
    button.frame = CGRectMake(0, 0, 44, 44);
    UIImage *image = [UIImage systemImageNamed:symbol];
    if (image)
        [button setImage:image forState:UIControlStateNormal];
    else
        [button setTitle:fallback forState:UIControlStateNormal];
    button.tintColor = UIColor.whiteColor;
    button.accessibilityLabel = accessibility;
    button.backgroundColor = UIColor.clearColor;
    button.layer.cornerRadius = 22;
    button.clipsToBounds = YES;
    UIVisualEffectView *glass = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    glass.userInteractionEnabled = NO;
    glass.frame = button.bounds;
    glass.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    glass.layer.cornerRadius = 22;
    glass.clipsToBounds = YES;
    [button insertSubview:glass atIndex:0];
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
- (void)configureTitle:(NSString *)title identifier:(NSString *)identifier iconPath:(NSString *)iconPath;
@end

@implementation Vita3KGameCell

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.glass = [[UIVisualEffectView alloc] initWithEffect:glass_effect()];
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
    self.titleLabel.textColor = UIColor.whiteColor;
    self.titleLabel.numberOfLines = 2;
    [self.glass.contentView addSubview:self.titleLabel];

    self.identifierLabel = [[UILabel alloc] init];
    self.identifierLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightMedium];
    self.identifierLabel.textColor = UIColor.secondaryLabelColor;
    [self.glass.contentView addSubview:self.identifierLabel];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat inset = 12;
    const CGFloat labelHeight = 70;
    self.icon.frame = CGRectMake(inset, inset, CGRectGetWidth(self.bounds) - inset * 2,
        CGRectGetHeight(self.bounds) - labelHeight - inset * 2);
    self.titleLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.icon.frame) + 7,
        CGRectGetWidth(self.bounds) - inset * 2, 44);
    self.identifierLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.titleLabel.frame),
        CGRectGetWidth(self.bounds) - inset * 2, 18);
}

- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    [UIView animateWithDuration:0.16 animations:^{
        self.transform = highlighted ? CGAffineTransformMakeScale(0.96, 0.96) : CGAffineTransformIdentity;
        self.alpha = highlighted ? 0.78 : 1.0;
    }];
}

- (void)configureTitle:(NSString *)title identifier:(NSString *)identifier iconPath:(NSString *)iconPath {
    self.titleLabel.text = title;
    self.identifierLabel.text = identifier;
    UIImage *image = iconPath.length ? [UIImage imageWithContentsOfFile:iconPath] : nil;
    if (iconPath.length && !image)
        LOG_ERROR("iOS library art could not be decoded at '{}'", iconPath.UTF8String);
    self.icon.image = image ?: [UIImage systemImageNamed:@"gamecontroller.fill"];
    self.icon.tintColor = UIColor.systemPinkColor;
    self.icon.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.55];
}

@end


@interface Vita3KSettingsView : UIView
@property(nonatomic) Vita3KIOSSettings values;
@property(nonatomic, strong) UIScrollView *scrollView;
@property(nonatomic, strong) UIStackView *stack;
@property(nonatomic, strong) UISlider *resolutionSlider;
@property(nonatomic, strong) UILabel *resolutionValue;
@property(nonatomic, strong) UISwitch *vsyncSwitch;
@property(nonatomic, strong) UISwitch *fpsSwitch;
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
    self.backgroundColor = [UIColor colorWithRed:0.025 green:0.03 blue:0.055 alpha:0.98];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    self.scrollView = [[UIScrollView alloc] initWithFrame:self.bounds];
    self.scrollView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentAlways;
    [self addSubview:self.scrollView];

    self.stack = [[UIStackView alloc] init];
    self.stack.axis = UILayoutConstraintAxisVertical;
    self.stack.spacing = 18;
    self.stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.scrollView addSubview:self.stack];
    [NSLayoutConstraint activateConstraints:@[
        [self.stack.leadingAnchor constraintEqualToAnchor:self.scrollView.frameLayoutGuide.leadingAnchor constant:20],
        [self.stack.trailingAnchor constraintEqualToAnchor:self.scrollView.frameLayoutGuide.trailingAnchor constant:-20],
        [self.stack.topAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.topAnchor constant:18],
        [self.stack.bottomAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.bottomAnchor constant:-30],
    ]];

    UIStackView *header = [[UIStackView alloc] init];
    header.axis = UILayoutConstraintAxisHorizontal;
    header.alignment = UIStackViewAlignmentCenter;
    UIButton *back = symbol_button(@"chevron.left", @"Back", @"Back to library");
    [back addTarget:self action:@selector(close) forControlEvents:UIControlEventTouchUpInside];
    [back.widthAnchor constraintEqualToConstant:46].active = YES;
    [back.heightAnchor constraintEqualToConstant:46].active = YES;
    UILabel *title = [[UILabel alloc] init];
    title.text = @"Settings";
    title.font = [UIFont systemFontOfSize:32 weight:UIFontWeightBold];
    title.textColor = UIColor.whiteColor;
    UIButton *save = [UIButton buttonWithType:UIButtonTypeSystem];
    [save setTitle:@"Save" forState:UIControlStateNormal];
    save.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [save addTarget:self action:@selector(save) forControlEvents:UIControlEventTouchUpInside];
    [header addArrangedSubview:back];
    [header addArrangedSubview:title];
    [header addArrangedSubview:save];
    [self.stack addArrangedSubview:header];

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
    [self addSection:@"Video" rows:@[
        [self row:@"Resolution multiplier" hint:@"Higher values are sharper but increase GPU load." accessory:resolutionAccessory],
        [self switchRow:@"V-Sync" hint:@"Synchronizes presentation to the display." value:values.v_sync output:&_vsyncSwitch],
        [self switchRow:@"FPS hack" hint:@"Forces some 30 FPS games to 60; it can break timing." value:values.fps_hack output:&_fpsSwitch],
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
    [controller setTitle:@"Open Controller Options" forState:UIControlStateNormal];
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
        [self row:@"Show battery %" hint:@"Device battery level while playing."
            accessory:[self defaultsSwitch:@"vita3k.perf.battery" defaults:defaults]],
    ]];
    return self;
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
    row.axis = UILayoutConstraintAxisHorizontal;
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 16;
    row.layoutMargins = UIEdgeInsetsMake(11, 14, 11, 14);
    row.layoutMarginsRelativeArrangement = YES;
    [labels setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [accessory setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
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
    // Plain blur, not interactive glass: live glass refraction on every
    // section made the settings scroll visibly stutter.
    UIVisualEffectView *glass = [[UIVisualEffectView alloc]
        initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
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
    action.settings.fps_hack = self.fpsSwitch.on;
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
}
@property(nonatomic, strong) UICollectionView *collectionView;
@property(nonatomic, strong) UILabel *emptyLabel;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) CAGradientLayer *backgroundGradient;
- (void)updateGames:(const std::vector<Vita3KIOSGameEntry> &)games settings:(const Vita3KIOSSettings &)settings;
@end

@implementation Vita3KLibraryView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.backgroundColor = [UIColor colorWithRed:0.02 green:0.025 blue:0.05 alpha:1];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundGradient = [CAGradientLayer layer];
    self.backgroundGradient.colors = @[
        (id)[UIColor colorWithRed:0.02 green:0.035 blue:0.09 alpha:1].CGColor,
        (id)[UIColor colorWithRed:0.14 green:0.035 blue:0.16 alpha:1].CGColor,
        (id)[UIColor colorWithRed:0.015 green:0.08 blue:0.11 alpha:1].CGColor,
    ];
    self.backgroundGradient.startPoint = CGPointMake(0, 0);
    self.backgroundGradient.endPoint = CGPointMake(1, 1);
    [self.layer insertSublayer:self.backgroundGradient atIndex:0];

    UILabel *title = [[UILabel alloc] init];
    title.text = @"Vita3K";
    title.font = [UIFont systemFontOfSize:38 weight:UIFontWeightBlack];
    title.textColor = UIColor.whiteColor;
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

    UICollectionViewFlowLayout *layout = [[UICollectionViewFlowLayout alloc] init];
    layout.minimumInteritemSpacing = 14;
    layout.minimumLineSpacing = 18;
    self.collectionView = [[UICollectionView alloc] initWithFrame:CGRectZero collectionViewLayout:layout];
    self.collectionView.backgroundColor = UIColor.clearColor;
    self.collectionView.dataSource = self;
    self.collectionView.delegate = self;
    self.collectionView.alwaysBounceVertical = YES;
    [self.collectionView registerClass:Vita3KGameCell.class forCellWithReuseIdentifier:@"game"];
    [self addSubview:self.collectionView];

    self.emptyLabel = [[UILabel alloc] init];
    self.emptyLabel.text = @"No games found\n\nIn Files, copy your working desktop Vita3K data folder into\nDocuments/Vita3K/vita, then tap Refresh.";
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
    title.frame = CGRectMake(safe.left + 22, safe.top + 14, 240, 50);
    settings.frame = CGRectMake(CGRectGetWidth(self.bounds) - safe.right - 58, safe.top + 17, 44, 44);
    refresh.frame = CGRectMake(CGRectGetMinX(settings.frame) - 54, safe.top + 17, 44, 44);
    self.statusLabel.frame = CGRectMake(CGRectGetMaxX(title.frame), safe.top + 20,
        MAX(0, CGRectGetMinX(refresh.frame) - CGRectGetMaxX(title.frame) - 10), 38);
    self.collectionView.frame = CGRectMake(safe.left + 18, safe.top + 78,
        CGRectGetWidth(self.bounds) - safe.left - safe.right - 36,
        CGRectGetHeight(self.bounds) - safe.top - safe.bottom - 88);
    self.emptyLabel.frame = CGRectInset(self.collectionView.frame, 40, 40);
}

- (void)updateGames:(const std::vector<Vita3KIOSGameEntry> &)games settings:(const Vita3KIOSSettings &)settings {
    _games = games;
    _settings = settings;
    self.emptyLabel.hidden = !_games.empty();
    self.collectionView.hidden = _games.empty();
    [self.collectionView reloadData];
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
    [cell configureTitle:title identifier:identifier iconPath:iconPath];
    return cell;
}

- (CGSize)collectionView:(UICollectionView *)collectionView layout:(UICollectionViewLayout *)layout sizeForItemAtIndexPath:(NSIndexPath *)indexPath {
    (void)layout;
    (void)indexPath;
    const CGFloat width = CGRectGetWidth(collectionView.bounds);
    const NSInteger columns = width >= 900 ? 4 : (width >= 620 ? 3 : 2);
    const CGFloat itemWidth = floor((width - (columns - 1) * 14) / columns);
    return CGSizeMake(itemWidth, itemWidth * 0.9 + 58);
}

- (void)collectionView:(UICollectionView *)collectionView didSelectItemAtIndexPath:(NSIndexPath *)indexPath {
    (void)collectionView;
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
    // Block further taps and make the boot visibly in progress; the whole
    // library view is removed once the emulator takes over the screen.
    self.userInteractionEnabled = NO;
    UIView *dim = [[UIView alloc] initWithFrame:self.bounds];
    dim.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    dim.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    dim.alpha = 0;

    UIVisualEffectView *panel = [[UIVisualEffectView alloc]
        initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
    panel.frame = CGRectMake(0, 0, 260, 130);
    panel.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    panel.autoresizingMask = UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin
        | UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    panel.layer.cornerRadius = 26;
    panel.clipsToBounds = YES;

    UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
    spinner.color = UIColor.whiteColor;
    spinner.center = CGPointMake(130, 48);
    [spinner startAnimating];
    [panel.contentView addSubview:spinner];

    UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(14, 82, 232, 36)];
    label.text = [NSString stringWithFormat:@"Booting %@…", title];
    label.textColor = UIColor.whiteColor;
    label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 2;
    label.adjustsFontSizeToFitWidth = YES;
    [panel.contentView addSubview:label];

    [dim addSubview:panel];
    [self addSubview:dim];
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

void vita3k_ios_show_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings) {
    const std::vector<Vita3KIOSGameEntry> gamesCopy = games;
    const Vita3KIOSSettings settingsCopy = settings;
    perform_on_main(^{
        UIWindow *window = active_window();
        if (!window)
            return;
        if (!g_library) {
            g_library = [[Vita3KLibraryView alloc] initWithFrame:window.bounds];
            [window addSubview:g_library];
        }
        [g_library updateGames:gamesCopy settings:settingsCopy];
        [window bringSubviewToFront:g_library];
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

static UIVisualEffectView *g_perf_hud = nil;
static UILabel *g_perf_label = nil;

void vita3k_ios_update_perf_overlay(const float guest_fps) {
    perform_on_main(^{
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        const BOOL show_fps = [defaults boolForKey:@"vita3k.perf.fps"];
        const BOOL show_ram = [defaults boolForKey:@"vita3k.perf.ram"];
        const BOOL show_battery = [defaults boolForKey:@"vita3k.perf.battery"];
        if (!show_fps && !show_ram && !show_battery) {
            g_perf_hud.hidden = YES;
            return;
        }

        UIWindow *window = active_window();
        if (!window)
            return;
        if (!g_perf_hud) {
            g_perf_hud = [[UIVisualEffectView alloc]
                initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
            g_perf_hud.layer.cornerRadius = 12;
            g_perf_hud.clipsToBounds = YES;
            g_perf_hud.userInteractionEnabled = NO;
            g_perf_label = [[UILabel alloc] init];
            g_perf_label.textColor = UIColor.whiteColor;
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
            UIDevice.currentDevice.batteryMonitoringEnabled = YES;
            const float level = UIDevice.currentDevice.batteryLevel;
            if (level >= 0)
                [parts addObject:[NSString stringWithFormat:@"%.0f%%", level * 100.0f]];
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

void vita3k_ios_report_settings_result(const std::vector<std::string> &restart_required) {
    NSMutableArray<NSString *> *labels = [NSMutableArray arrayWithCapacity:restart_required.size()];
    for (const auto &label : restart_required)
        [labels addObject:[NSString stringWithUTF8String:label.c_str()]];
    perform_on_main(^{
        [g_library reportRestartRequired:labels];
    });
}
