// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#include <vita3k_ios/NativeFrontend.h>
#include <vita3k_ios/VirtualController.h>

#import <UIKit/UIKit.h>

#include <mutex>

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

UIVisualEffect *glass_effect() {
    Class glassClass = NSClassFromString(@"UIGlassEffect");
    SEL selector = NSSelectorFromString(@"effectWithStyle:");
    if (glassClass && [glassClass respondsToSelector:selector]) {
        using Factory = id (*)(id, SEL, NSInteger);
        Factory factory = reinterpret_cast<Factory>([glassClass methodForSelector:selector]);
        id effect = factory(glassClass, selector, 0); // UIGlassEffectStyleRegular
        @try {
            [effect setValue:@YES forKey:@"interactive"];
        } @catch (__unused NSException *exception) {
        }
        return effect;
    }
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
}

UIButton *symbol_button(NSString *symbol, NSString *fallback, NSString *accessibility) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    UIImage *image = [UIImage systemImageNamed:symbol];
    if (image)
        [button setImage:image forState:UIControlStateNormal];
    else
        [button setTitle:fallback forState:UIControlStateNormal];
    button.tintColor = UIColor.whiteColor;
    button.accessibilityLabel = accessibility;
    button.backgroundColor = [UIColor colorWithWhite:1 alpha:0.08];
    button.layer.cornerRadius = 22;
    return button;
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
    const CGFloat labelHeight = 58;
    self.icon.frame = CGRectMake(inset, inset, CGRectGetWidth(self.bounds) - inset * 2,
        CGRectGetHeight(self.bounds) - labelHeight - inset * 2);
    self.titleLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.icon.frame) + 7,
        CGRectGetWidth(self.bounds) - inset * 2, 32);
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
    return self;
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
    UIVisualEffectView *glass = [[UIVisualEffectView alloc] initWithEffect:glass_effect()];
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
- (void)updateGames:(const std::vector<Vita3KIOSGameEntry> &)games settings:(const Vita3KIOSSettings &)settings;
@end

@implementation Vita3KLibraryView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.backgroundColor = [UIColor colorWithRed:0.02 green:0.025 blue:0.05 alpha:1];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

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
    [cell configureTitle:[NSString stringWithUTF8String:game.title.c_str()]
        identifier:[NSString stringWithUTF8String:game.title_id.c_str()]
        iconPath:[NSString stringWithUTF8String:game.icon_path.c_str()]];
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
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::Launch;
    action.app_path = _games.at(static_cast<std::size_t>(indexPath.item)).app_path;
    queue_action(std::move(action));
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
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *window = active_window();
        if (!window)
            return;
        if (!g_library) {
            g_library = [[Vita3KLibraryView alloc] initWithFrame:window.bounds];
            [window addSubview:g_library];
        }
        [g_library updateGames:games settings:settings];
        [window bringSubviewToFront:g_library];
    });
}

void vita3k_ios_update_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [g_library updateGames:games settings:settings];
    });
}

void vita3k_ios_hide_library() {
    dispatch_async(dispatch_get_main_queue(), ^{
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

void vita3k_ios_report_settings_result(const std::vector<std::string> &restart_required) {
    NSMutableArray<NSString *> *labels = [NSMutableArray arrayWithCapacity:restart_required.size()];
    for (const auto &label : restart_required)
        [labels addObject:[NSString stringWithUTF8String:label.c_str()]];
    dispatch_async(dispatch_get_main_queue(), ^{
        [g_library reportRestartRequired:labels];
    });
}
