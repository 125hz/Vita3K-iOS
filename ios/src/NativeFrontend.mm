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
#import <GameController/GameController.h>
#import <PhotosUI/PhotosUI.h>
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

} // namespace

static void set_metal_drawables_hidden(UIWindow *window, BOOL hidden);

namespace {

UIWindow *active_window() {
    UIWindow *fallback = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]
            || scene.activationState == UISceneActivationStateUnattached)
            continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            if (window.isKeyWindow)
                return window;
            if (!fallback)
                fallback = window;
        }
    }
    // Session teardown can briefly leave no key window; the library must still
    // find a home instead of silently not appearing (black screen after quit).
    return fallback;
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

// Library metadata visibility toggles; both default ON.
BOOL show_game_version() {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    return [defaults objectForKey:@"tsubomi.showVersion"] ? [defaults boolForKey:@"tsubomi.showVersion"] : YES;
}

BOOL show_game_size() {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    return [defaults objectForKey:@"tsubomi.showGameSize"] ? [defaults boolForKey:@"tsubomi.showGameSize"] : YES;
}

// Extra metadata lines under the played-time line (version and size get their
// own lines; the trophy badge rides on the played line).
NSInteger metadata_line_count() {
    return 1 + (show_game_version() ? 1 : 0) + (show_game_size() ? 1 : 0);
}

NSString *tsubomi_app_version() {
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return version.length ? version : @"0.5.0";
}

NSString *played_time_text(const Vita3KIOSGameEntry &game) {
    const long long minutes = MAX(0, game.time_played_seconds) / 60;
    return game.time_played_seconds > 0 && minutes == 0
        ? @"<1m"
        : minutes >= 60
        ? [NSString stringWithFormat:@"%lldh %lldm", minutes / 60, minutes % 60]
        : [NSString stringWithFormat:@"%lldm", minutes];
}

NSString *last_played_text(const Vita3KIOSGameEntry &game) {
    if (game.last_played_timestamp <= 0)
        return @"Never played";
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.dateStyle = NSDateFormatterShortStyle;
    formatter.timeStyle = NSDateFormatterShortStyle;
    return [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:game.last_played_timestamp]];
}

NSString *game_size_text(const Vita3KIOSGameEntry &game) {
    NSByteCountFormatter *bytes = [[NSByteCountFormatter alloc] init];
    bytes.countStyle = NSByteCountFormatterCountStyleFile;
    return [bytes stringFromByteCount:(long long)game.size_bytes];
}

// Multi-line metadata: optional "v1.01" line, then "played · last [🏆 n/m]",
// then an optional size line — matching the library settings toggles.
NSAttributedString *game_metadata(const Vita3KIOSGameEntry &game) {
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    NSDictionary *plain = @{
        NSFontAttributeName: [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1],
        NSForegroundColorAttributeName: UIColor.secondaryLabelColor,
    };
    auto append = [&](NSString *line) {
        if (text.length)
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:plain]];
        [text appendAttributedString:[[NSAttributedString alloc] initWithString:line attributes:plain]];
    };
    if (show_game_version()) {
        NSString *versionText = [NSString stringWithUTF8String:game.version.c_str()];
        append(game.version.empty() || !versionText ? @"Unknown version" : [@"v" stringByAppendingString:versionText]);
    }
    append([NSString stringWithFormat:@"%@  ·  %@", played_time_text(game), last_played_text(game)]);
    if (game.trophies_total > 0) {
        [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"  " attributes:plain]];
        NSTextAttachment *trophy = [[NSTextAttachment alloc] init];
        UIFont *font = plain[NSFontAttributeName];
        trophy.image = [[UIImage systemImageNamed:@"trophy.fill"]
            imageWithTintColor:UIColor.systemYellowColor renderingMode:UIImageRenderingModeAlwaysOriginal];
        trophy.bounds = CGRectMake(0, font.descender, font.capHeight * 1.15, font.capHeight * 1.15);
        [text appendAttributedString:[NSAttributedString attributedStringWithAttachment:trophy]];
        [text appendAttributedString:[[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@" %d/%d", game.trophies_unlocked, game.trophies_total]
                attributes:plain]];
    }
    if (show_game_size())
        append(game_size_text(game));
    return text;
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

// ---- Custom cover art -------------------------------------------------------
// Users can replace a game's library art with a photo. The picked original is
// kept so the crop can be re-adjusted later; the rendered square cover is what
// cells actually display.

NSString *covers_directory() {
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *directory = [documents stringByAppendingPathComponent:@"Tsubomi/covers"];
    [NSFileManager.defaultManager createDirectoryAtPath:directory
                            withIntermediateDirectories:YES attributes:nil error:nil];
    return directory;
}

NSString *cover_original_path(NSString *titleId) {
    return [covers_directory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"%@-original.png", titleId]];
}

NSString *cover_render_path(NSString *titleId) {
    return [covers_directory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"%@-cover.png", titleId]];
}

BOOL has_custom_cover(NSString *titleId) {
    return [NSFileManager.defaultManager fileExistsAtPath:cover_render_path(titleId)];
}

// ---- Per-game settings ------------------------------------------------------
// Overrides are a defaults dictionary per title; absent = use global settings.

NSString *game_settings_key(NSString *titleId) {
    return [@"tsubomi.gameSettings." stringByAppendingString:titleId ?: @""];
}

BOOL has_game_settings(NSString *titleId) {
    return [NSUserDefaults.standardUserDefaults dictionaryForKey:game_settings_key(titleId)] != nil;
}

Vita3KIOSSettings game_settings_or(NSString *titleId, const Vita3KIOSSettings &fallback) {
    NSDictionary *stored = [NSUserDefaults.standardUserDefaults dictionaryForKey:game_settings_key(titleId)];
    if (!stored)
        return fallback;
    Vita3KIOSSettings settings = fallback;
    if (stored[@"resolution"])
        settings.resolution_multiplier = [stored[@"resolution"] floatValue];
    if (stored[@"vsync"])
        settings.v_sync = [stored[@"vsync"] boolValue];
    settings.fps_limit = 60;
    if (stored[@"cpuOpt"])
        settings.cpu_opt = [stored[@"cpuOpt"] boolValue];
    if (stored[@"ngs"])
        settings.ngs_enable = [stored[@"ngs"] boolValue];
    if (stored[@"asyncPipelines"])
        settings.async_pipeline_compilation = [stored[@"asyncPipelines"] boolValue];
    if (stored[@"anisotropic"])
        settings.anisotropic_filtering = [stored[@"anisotropic"] intValue];
    if (stored[@"highAccuracy"])
        settings.high_accuracy = [stored[@"highAccuracy"] boolValue];
    if (stored[@"surfaceSync"])
        settings.surface_sync = [stored[@"surfaceSync"] boolValue];
    return settings;
}

void store_game_settings(NSString *titleId, const Vita3KIOSSettings &settings) {
    [NSUserDefaults.standardUserDefaults setObject:@{
        @"resolution": @(settings.resolution_multiplier),
        @"vsync": @(settings.v_sync),
        @"cpuOpt": @(settings.cpu_opt),
        @"ngs": @(settings.ngs_enable),
        @"asyncPipelines": @(settings.async_pipeline_compilation),
        @"anisotropic": @(settings.anisotropic_filtering),
        @"highAccuracy": @(settings.high_accuracy),
        @"surfaceSync": @(settings.surface_sync),
    } forKey:game_settings_key(titleId)];
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

// Pan/zoom square crop for custom cover art. The scroll view's visible square
// maps directly to the rendered 1024x1024 cover.
@interface Vita3KCoverCropController : UIViewController <UIScrollViewDelegate>
@property(nonatomic, strong) UIImage *image;
@property(nonatomic, copy) NSString *titleId;
@property(nonatomic, copy) dispatch_block_t onDone;
@property(nonatomic, strong) UIScrollView *cropScroll;
@property(nonatomic, strong) UIImageView *imageView;
@end

@implementation Vita3KCoverCropController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    self.cropScroll = [[UIScrollView alloc] init];
    self.cropScroll.delegate = self;
    self.cropScroll.showsHorizontalScrollIndicator = NO;
    self.cropScroll.showsVerticalScrollIndicator = NO;
    self.cropScroll.alwaysBounceHorizontal = YES;
    self.cropScroll.alwaysBounceVertical = YES;
    self.cropScroll.layer.borderColor = UIColor.whiteColor.CGColor;
    self.cropScroll.layer.borderWidth = 1.5;
    self.cropScroll.clipsToBounds = YES;
    [self.view addSubview:self.cropScroll];

    self.imageView = [[UIImageView alloc] initWithImage:self.image];
    self.imageView.contentMode = UIViewContentModeScaleToFill;
    [self.cropScroll addSubview:self.imageView];

    UILabel *hint = [[UILabel alloc] init];
    hint.text = @"Pinch and drag to frame the cover";
    hint.textColor = UIColor.whiteColor;
    hint.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    hint.textAlignment = NSTextAlignmentCenter;
    hint.tag = 401;
    [self.view addSubview:hint];

    UIButton *cancel = [UIButton buttonWithType:UIButtonTypeSystem];
    [cancel setTitle:@"Cancel" forState:UIControlStateNormal];
    cancel.tintColor = UIColor.whiteColor;
    cancel.tag = 402;
    [cancel addTarget:self action:@selector(cancelTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:cancel];

    UIButton *save = [UIButton buttonWithType:UIButtonTypeSystem];
    [save setTitle:@"Save" forState:UIControlStateNormal];
    save.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    save.tag = 403;
    [save addTarget:self action:@selector(saveTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:save];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const UIEdgeInsets safe = self.view.safeAreaInsets;
    const CGRect bounds = self.view.bounds;
    const CGFloat side = MIN(CGRectGetWidth(bounds) - 40,
        CGRectGetHeight(bounds) - safe.top - safe.bottom - 140);
    const BOOL firstLayout = CGRectIsEmpty(self.cropScroll.frame);
    self.cropScroll.frame = CGRectMake((CGRectGetWidth(bounds) - side) / 2,
        safe.top + 64, side, side);
    [self.view viewWithTag:401].frame = CGRectMake(20, safe.top + 18, CGRectGetWidth(bounds) - 40, 24);
    [self.view viewWithTag:402].frame = CGRectMake(24, CGRectGetMaxY(self.cropScroll.frame) + 18, 90, 44);
    [self.view viewWithTag:403].frame = CGRectMake(CGRectGetWidth(bounds) - 114,
        CGRectGetMaxY(self.cropScroll.frame) + 18, 90, 44);
    if (firstLayout && self.image) {
        const CGSize imageSize = self.image.size;
        // Minimum zoom always fills the square.
        const CGFloat fill = MAX(side / imageSize.width, side / imageSize.height);
        self.imageView.frame = CGRectMake(0, 0, imageSize.width, imageSize.height);
        self.cropScroll.contentSize = imageSize;
        self.cropScroll.minimumZoomScale = fill;
        self.cropScroll.maximumZoomScale = MAX(fill * 8, 2.0);
        self.cropScroll.zoomScale = fill;
        // Center the initial crop.
        self.cropScroll.contentOffset = CGPointMake(
            MAX(0, (imageSize.width * fill - side) / 2),
            MAX(0, (imageSize.height * fill - side) / 2));
    }
}

- (UIView *)viewForZoomingInScrollView:(UIScrollView *)scrollView {
    (void)scrollView;
    return self.imageView;
}

- (void)cancelTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)saveTapped {
    const CGFloat side = CGRectGetWidth(self.cropScroll.bounds);
    const CGFloat zoom = self.cropScroll.zoomScale;
    const CGRect cropInImage = CGRectMake(self.cropScroll.contentOffset.x / zoom,
        self.cropScroll.contentOffset.y / zoom, side / zoom, side / zoom);
    const CGFloat renderSide = 1024;
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
    format.opaque = YES;
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc]
        initWithSize:CGSizeMake(renderSide, renderSide) format:format];
    UIImage *image = self.image;
    UIImage *rendered = [renderer imageWithActions:^(__unused UIGraphicsImageRendererContext *context) {
        const CGFloat scale = renderSide / cropInImage.size.width;
        [image drawInRect:CGRectMake(-cropInImage.origin.x * scale, -cropInImage.origin.y * scale,
            image.size.width * scale, image.size.height * scale)];
    }];
    [UIImagePNGRepresentation(rendered) writeToFile:cover_render_path(self.titleId) atomically:YES];
    dispatch_block_t done = self.onDone;
    [self dismissViewControllerAnimated:YES completion:^{
        if (done)
            done();
    }];
}

@end

// Keeps the PHPicker delegate alive while the sheet is up; on pick, stores the
// original and opens the crop controller.
@interface Vita3KCoverPicker : NSObject <PHPickerViewControllerDelegate>
@property(nonatomic, copy) NSString *titleId;
@end

static Vita3KCoverPicker *g_cover_picker = nil;
static void present_cover_crop(NSString *titleId, UIImage *image);

@implementation Vita3KCoverPicker

- (void)picker:(PHPickerViewController *)picker didFinishPicking:(NSArray<PHPickerResult *> *)results {
    NSString *titleId = self.titleId;
    [picker dismissViewControllerAnimated:YES completion:nil];
    NSItemProvider *provider = results.firstObject.itemProvider;
    if (![provider canLoadObjectOfClass:UIImage.class])
        return;
    [provider loadObjectOfClass:UIImage.class completionHandler:^(id<NSItemProviderReading> object, NSError *error) {
        UIImage *image = (UIImage *)object;
        if (!image || error)
            return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [UIImagePNGRepresentation(image) writeToFile:cover_original_path(titleId) atomically:YES];
            present_cover_crop(titleId, image);
        });
    }];
}

@end

static void present_cover_crop(NSString *titleId, UIImage *image) {
    UIViewController *root = active_window().rootViewController;
    if (!root || !image)
        return;
    Vita3KCoverCropController *crop = [[Vita3KCoverCropController alloc] init];
    crop.image = image;
    crop.titleId = titleId;
    crop.onDone = ^{ reload_library_cells(); };
    crop.modalPresentationStyle = UIModalPresentationFullScreen;
    [root presentViewController:crop animated:YES completion:nil];
}

static void present_cover_picker(NSString *titleId) {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    PHPickerConfiguration *configuration = [[PHPickerConfiguration alloc] init];
    configuration.selectionLimit = 1;
    configuration.filter = PHPickerFilter.imagesFilter;
    PHPickerViewController *picker = [[PHPickerViewController alloc] initWithConfiguration:configuration];
    if (!g_cover_picker)
        g_cover_picker = [[Vita3KCoverPicker alloc] init];
    g_cover_picker.titleId = titleId;
    picker.delegate = g_cover_picker;
    [root presentViewController:picker animated:YES completion:nil];
}

@interface Vita3KGameCell : UICollectionViewCell
@property(nonatomic, strong) UIVisualEffectView *glass;
@property(nonatomic, strong) UIImageView *icon;
@property(nonatomic, strong) UILabel *titleLabel;
@property(nonatomic, strong) UILabel *identifierLabel;
@property(nonatomic, strong) UILabel *metadataLabel;
@property(nonatomic, strong) UIView *separator;
@property(nonatomic) BOOL listMode;
// Landscape card view: a centered cover-flow item — bare cover art with the
// title and play info centered beneath it, no glass card.
@property(nonatomic) BOOL carouselMode;
- (void)configureTitle:(NSString *)title identifier:(NSString *)identifier metadata:(NSAttributedString *)metadata
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
    self.metadataLabel.tintColor = UIColor.systemYellowColor;
    self.metadataLabel.tintAdjustmentMode = UIViewTintAdjustmentModeNormal;
    self.metadataLabel.numberOfLines = 0;
    [self.glass.contentView addSubview:self.metadataLabel];
    self.separator = [[UIView alloc] init];
    self.separator.backgroundColor = UIColor.separatorColor;
    [self.glass.contentView addSubview:self.separator];
    return self;
}

// Shared with the layout size calculation in Vita3KLibraryView so cells and
// their flow-layout heights always agree with the metadata toggles.
CGFloat library_metadata_height() {
    return metadata_line_count() * 16 + 2;
}

CGFloat library_list_row_height() {
    return MAX(64, 6 + 24 + (show_title_ids() ? 17 : 0) + library_metadata_height() + 10);
}

CGFloat library_card_label_height() {
    return 7 + 40 + (show_title_ids() ? 17 : 0) + library_metadata_height() + 10;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat inset = 12;
    if (self.carouselMode) {
        const CGFloat side = CGRectGetWidth(self.bounds);
        self.icon.frame = CGRectMake(0, 0, side, side);
        self.titleLabel.frame = CGRectMake(0, side + 10, side, 24);
        self.identifierLabel.frame = CGRectZero;
        self.metadataLabel.frame = CGRectMake(0, CGRectGetMaxY(self.titleLabel.frame) + 2, side, 18);
        self.separator.frame = CGRectZero;
        return;
    }
    if (self.listMode) {
        const CGFloat iconSize = MIN(72, MAX(1, CGRectGetHeight(self.bounds) - 20));
        self.icon.frame = CGRectMake(10, (CGRectGetHeight(self.bounds) - iconSize) / 2, iconSize, iconSize);
        const CGFloat textX = CGRectGetMaxX(self.icon.frame) + 13;
        const CGFloat textWidth = MAX(1, CGRectGetWidth(self.bounds) - textX - inset);
        self.titleLabel.frame = CGRectMake(textX, 6, textWidth, 24);
        self.identifierLabel.frame = CGRectMake(textX, CGRectGetMaxY(self.titleLabel.frame), textWidth, 17);
        self.metadataLabel.frame = CGRectMake(textX,
            (self.identifierLabel.hidden ? CGRectGetMaxY(self.titleLabel.frame) : CGRectGetMaxY(self.identifierLabel.frame)) + 2,
            textWidth, library_metadata_height());
        self.separator.frame = CGRectMake(textX, CGRectGetHeight(self.bounds) - 1,
            MAX(1, CGRectGetWidth(self.bounds) - textX), 1);
    } else {
        const CGFloat labelHeight = library_card_label_height();
        self.icon.frame = CGRectMake(inset, inset, CGRectGetWidth(self.bounds) - inset * 2,
            MAX(1, CGRectGetHeight(self.bounds) - labelHeight - inset * 2));
        self.titleLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.icon.frame) + 7,
            CGRectGetWidth(self.bounds) - inset * 2, 40);
        self.identifierLabel.frame = CGRectMake(inset, CGRectGetMaxY(self.titleLabel.frame),
            CGRectGetWidth(self.bounds) - inset * 2, 17);
        self.metadataLabel.frame = CGRectMake(inset,
            (self.identifierLabel.hidden ? CGRectGetMaxY(self.titleLabel.frame) : CGRectGetMaxY(self.identifierLabel.frame)) + 2,
            CGRectGetWidth(self.bounds) - inset * 2, library_metadata_height());
        self.separator.frame = CGRectZero;
    }
}

- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    if (self.carouselMode)
        return; // the carousel owns transform and alpha
    [UIView animateWithDuration:0.16 animations:^{
        self.transform = highlighted ? CGAffineTransformMakeScale(0.96, 0.96) : CGAffineTransformIdentity;
        self.alpha = highlighted ? 0.78 : 1.0;
    }];
}

- (void)configureTitle:(NSString *)title identifier:(NSString *)identifier metadata:(NSAttributedString *)metadata
              iconPath:(NSString *)iconPath listMode:(BOOL)listMode {
    self.listMode = listMode;
    self.titleLabel.text = title;
    self.identifierLabel.text = identifier;
    self.metadataLabel.attributedText = metadata;
    self.identifierLabel.hidden = !show_title_ids() || self.carouselMode;
    self.separator.hidden = !listMode;
    // List rows sit directly on the background like a system list: no
    // material, no forced palette — every color adapts to light/dark mode.
    // Carousel items are bare covers with no material at all.
    self.glass.effect = (listMode || self.carouselMode) ? nil : glass_effect(NO);
    self.glass.backgroundColor = UIColor.clearColor;
    self.glass.layer.cornerRadius = (listMode || self.carouselMode) ? 0 : 26;
    self.icon.layer.cornerRadius = listMode ? 8 : 18;
    self.titleLabel.numberOfLines = (listMode || self.carouselMode) ? 1 : 2;
    self.titleLabel.textAlignment = self.carouselMode ? NSTextAlignmentCenter : NSTextAlignmentLeft;
    self.metadataLabel.textAlignment = self.carouselMode ? NSTextAlignmentCenter : NSTextAlignmentLeft;
    if (!self.carouselMode) {
        self.transform = CGAffineTransformIdentity;
        self.alpha = 1.0;
    }
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
@property(nonatomic, strong) UISwitch *cpuSwitch;
@property(nonatomic, strong) UISwitch *ngsSwitch;
@property(nonatomic, strong) UISwitch *asyncSwitch;
@property(nonatomic, strong) UISwitch *highAccuracySwitch;
@property(nonatomic, strong) UISwitch *surfaceSyncSwitch;
@property(nonatomic, strong) UISegmentedControl *anisotropicControl;
@property(nonatomic, strong) UILabel *headerTitle;
@property(nonatomic, strong) NSDictionary<NSString *, NSArray<UIView *> *> *pages;
@property(nonatomic) BOOL showingRoot;
// Non-nil when editing one title's override instead of the global settings.
@property(nonatomic, copy) NSString *perGameTitleId;
- (instancetype)initWithFrame:(CGRect)frame values:(const Vita3KIOSSettings &)values;
- (void)enterPerGameModeForTitle:(NSString *)titleId;
- (void)navigateBack;
- (void)padSwitchCategory:(NSInteger)delta;
@end

@implementation Vita3KSettingsView

- (void)updateOpaqueBackground {
    UIColor *background = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark
        ? UIColor.blackColor
        : UIColor.whiteColor;
    self.backgroundColor = background;
    self.scrollView.backgroundColor = background;
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self updateOpaqueBackground];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    [super traitCollectionDidChange:previousTraitCollection];
    if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle)
        [self updateOpaqueBackground];
}

- (instancetype)initWithFrame:(CGRect)frame values:(const Vita3KIOSSettings &)values {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.values = values;
    // Use a resolved opaque color. A translucent/material settings root can
    // otherwise reveal the last rendered game frame underneath the library.
    self.backgroundColor = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark
        ? UIColor.blackColor
        : UIColor.whiteColor;
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
    [back addTarget:self action:@selector(navigateBack) forControlEvents:UIControlEventTouchUpInside];
    [back.widthAnchor constraintEqualToConstant:46].active = YES;
    [back.heightAnchor constraintEqualToConstant:46].active = YES;
    self.headerTitle = [[UILabel alloc] init];
    self.headerTitle.text = @"Settings";
    self.headerTitle.font = [UIFont systemFontOfSize:32 weight:UIFontWeightBold];
    self.headerTitle.textColor = UIColor.labelColor;
    UIButton *save = [UIButton buttonWithType:UIButtonTypeSystem];
    [save setTitle:@"Save" forState:UIControlStateNormal];
    save.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [save addTarget:self action:@selector(save) forControlEvents:UIControlEventTouchUpInside];
    save.translatesAutoresizingMaskIntoConstraints = NO;
    [header addArrangedSubview:back];
    [header addArrangedSubview:self.headerTitle];
    [self addSubview:header];
    // Save is pinned to the safe-area trailing edge on its own, outside the
    // header stack: stack distribution let it wander as the title text and
    // orientation changed.
    [self addSubview:save];
    [self.headerTitle setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [self.headerTitle setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [header.leadingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor constant:20],
        [header.trailingAnchor constraintLessThanOrEqualToAnchor:save.leadingAnchor constant:-12],
        [header.topAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.topAnchor constant:8],
        [header.heightAnchor constraintEqualToConstant:50],
        [save.trailingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor constant:-20],
        [save.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],
        [self.scrollView.leadingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor],
        [self.scrollView.trailingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor],
        [self.scrollView.topAnchor constraintEqualToAnchor:header.bottomAnchor constant:8],
        [self.scrollView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];

    self.resolutionSlider = [[UISlider alloc] init];
    // 0.5x/0.75x render below native for heavy titles.
    self.resolutionSlider.minimumValue = 0.5f;
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

    [self addSection:@"Video" rows:@[
        [self switchRow:@"V-Sync" hint:@"Synchronizes presentation to the display." value:values.v_sync output:&_vsyncSwitch],
    ]];

    self.anisotropicControl = [[UISegmentedControl alloc] initWithItems:@[@"Off", @"2x", @"4x", @"8x", @"16x"]];
    const int anisotropicValues[] = {1, 2, 4, 8, 16};
    self.anisotropicControl.selectedSegmentIndex = 0;
    for (NSInteger index = 0; index < 5; ++index) {
        if (values.anisotropic_filtering == anisotropicValues[index])
            self.anisotropicControl.selectedSegmentIndex = index;
    }
    [self addSection:@"Graphics" rows:@[
        [self row:@"Resolution multiplier" hint:@"Higher values are sharper but increase GPU load; below 1x renders faster." accessory:resolutionAccessory],
        [self switchRow:@"High accuracy" hint:@"Uses slower but more accurate render paths. Try this if a game's graphics look broken." value:values.high_accuracy output:&_highAccuracySwitch],
        [self switchRow:@"Surface sync" hint:@"Synchronizes render targets with guest-visible surface data. Enable this if a game's lighting appears white or missing." value:values.surface_sync output:&_surfaceSyncSwitch],
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
        [self row:@"Show frametime" hint:@"Average guest frame duration in milliseconds."
            accessory:[self defaultsSwitch:@"vita3k.perf.frametime" defaults:defaults]],
        [self row:@"Show frametime graph" hint:@"Minimal history graph; white in dark mode and black in light mode."
            accessory:[self defaultsSwitch:@"vita3k.perf.frametimeGraph" defaults:defaults]],
        [self row:@"Show RAM usage" hint:@"This app's physical memory footprint."
            accessory:[self defaultsSwitch:@"vita3k.perf.ram" defaults:defaults]],
        [self row:@"Show battery %" hint:@"Current device battery level."
            accessory:[self defaultsSwitch:@"vita3k.perf.battery" defaults:defaults]],
    ]];

    UISwitch *titleIds = [[UISwitch alloc] init];
    titleIds.on = show_title_ids();
    [titleIds addTarget:self action:@selector(showTitleIdsChanged:) forControlEvents:UIControlEventValueChanged];
    UISwitch *versionToggle = [self defaultsReloadSwitch:@"tsubomi.showVersion" on:show_game_version()];
    UISwitch *sizeToggle = [self defaultsReloadSwitch:@"tsubomi.showGameSize" on:show_game_size()];
    [self addSection:@"Library" rows:@[
        [self row:@"Show title IDs" hint:@"Show the PCSG… identifier under each game." accessory:titleIds],
        [self row:@"Show version number" hint:@"Show the installed game version under the title ID." accessory:versionToggle],
        [self row:@"Show game size" hint:@"Show each game's installed size on its own line." accessory:sizeToggle],
    ]];

    UIButton *changelog = [UIButton buttonWithType:UIButtonTypeSystem];
    [changelog setTitle:@"Changelog" forState:UIControlStateNormal];
    changelog.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [changelog addTarget:self action:@selector(showChangelog) forControlEvents:UIControlEventTouchUpInside];
    UIButton *forkLink = [UIButton buttonWithType:UIButtonTypeSystem];
    [forkLink setTitle:@"Vita3K ↗" forState:UIControlStateNormal];
    forkLink.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [forkLink addTarget:self action:@selector(openVita3K) forControlEvents:UIControlEventTouchUpInside];
    UIButton *developerLink = [UIButton buttonWithType:UIButtonTypeSystem];
    [developerLink setTitle:@"@halcyonpalace" forState:UIControlStateNormal];
    developerLink.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [developerLink addTarget:self action:@selector(openDeveloperProfile) forControlEvents:UIControlEventTouchUpInside];
    [self addSection:@"About" rows:@[
        [self row:@"Version" hint:@"Tsubomi — an iOS PS Vita emulator." accessory:[self valueLabel:tsubomi_app_version()]],
        [self row:@"What's new" hint:@"Changes in this version." accessory:changelog],
        [self row:@"Forked from" hint:@"Tsubomi is built on the Vita3K emulator." accessory:forkLink],
        [self row:@"Developed by" hint:@"twitter / discord" accessory:developerLink],
        [self row:@"Thanks to" hint:@"" accessory:[self valueLabel:@"Bloom, Craig, Thomasina"]],
    ]];
    NSArray<UIView *> *sections = [self.stack.arrangedSubviews copy];
    self.pages = @{
        @"Video": @[sections[0]],
        @"Graphics": @[sections[1]],
        @"Audio": @[sections[2]],
        @"System & Input": @[sections[3]],
        @"Performance Overlay": @[sections[4]],
        @"Library": @[sections[5]],
        @"About": @[sections[6]],
    };
    [self showSettingsRoot];
    return self;
}

- (void)clearSettingsStack {
    for (UIView *view in [self.stack.arrangedSubviews copy]) {
        [self.stack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
}

// One flat page with the emulator sections (no frontend-only toggles); Save
// writes the override for this title only.
- (void)enterPerGameModeForTitle:(NSString *)titleId {
    self.perGameTitleId = titleId;
    [self clearSettingsStack];
    self.showingRoot = NO;
    self.headerTitle.text = [NSString stringWithFormat:@"%@ settings", titleId];
    for (NSString *category in @[@"Video", @"Graphics", @"Audio", @"System & Input"])
        for (UIView *view in self.pages[category])
            [self.stack addArrangedSubview:view];
    UIButton *useGlobal = [UIButton buttonWithType:UIButtonTypeSystem];
    [useGlobal setTitle:@"Use global settings" forState:UIControlStateNormal];
    useGlobal.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [useGlobal setTitleColor:UIColor.systemRedColor forState:UIControlStateNormal];
    [useGlobal.heightAnchor constraintEqualToConstant:48].active = YES;
    [useGlobal addTarget:self action:@selector(clearPerGameSettings) forControlEvents:UIControlEventTouchUpInside];
    [self.stack addArrangedSubview:useGlobal];
}

- (void)clearPerGameSettings {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:game_settings_key(self.perGameTitleId)];
    [self close];
}

- (UIButton *)categoryButton:(NSString *)title symbol:(NSString *)symbol color:(UIColor *)color {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.accessibilityIdentifier = title;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    button.titleLabel.font = [UIFont systemFontOfSize:19 weight:UIFontWeightSemibold];
    [button setTitle:[NSString stringWithFormat:@"  %@", title] forState:UIControlStateNormal];
    [button setTitleColor:UIColor.labelColor forState:UIControlStateNormal];
    [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    button.tintColor = color;
    button.backgroundColor = UIColor.secondarySystemBackgroundColor;
    button.layer.cornerRadius = 20;
    button.contentEdgeInsets = UIEdgeInsetsMake(0, 18, 0, 18);
    [button.heightAnchor constraintEqualToConstant:68].active = YES;
    [button addTarget:self action:@selector(openCategory:) forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)showSettingsRoot {
    [self clearSettingsStack];
    self.showingRoot = YES;
    self.headerTitle.text = @"Settings";
    NSArray<NSArray *> *categories = @[
        @[@"Video", @"play.rectangle.fill", UIColor.systemBlueColor],
        @[@"Graphics", @"sparkles", UIColor.systemPurpleColor],
        @[@"Audio", @"speaker.wave.2.fill", UIColor.systemOrangeColor],
        @[@"System & Input", @"gamecontroller.fill", UIColor.systemGreenColor],
        @[@"Performance Overlay", @"gauge.with.dots.needle.67percent", UIColor.systemPinkColor],
        @[@"Library", @"square.grid.2x2.fill", UIColor.systemTealColor],
        @[@"Submit bugs", @"ladybug.fill", UIColor.systemRedColor],
    ];
    for (NSArray *category in categories)
        [self.stack addArrangedSubview:[self categoryButton:category[0] symbol:category[1] color:category[2]]];

    UIButton *about = [UIButton buttonWithType:UIButtonTypeSystem];
    about.accessibilityIdentifier = @"About";
    [about setTitle:[NSString stringWithFormat:@"About  ·  Tsubomi %@", tsubomi_app_version()]
            forState:UIControlStateNormal];
    // Blue reads as tappable — this row opens the About page.
    [about setTitleColor:UIColor.systemBlueColor forState:UIControlStateNormal];
    about.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    about.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    [about addTarget:self action:@selector(openCategory:) forControlEvents:UIControlEventTouchUpInside];
    [about.heightAnchor constraintEqualToConstant:36].active = YES;
    [self.stack addArrangedSubview:about];
}

- (void)openCategory:(UIButton *)sender {
    [self openCategoryNamed:sender.accessibilityIdentifier];
}

- (void)openCategoryNamed:(NSString *)category {
    if ([category isEqualToString:@"Submit bugs"]) {
        [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"https://forms.gle/PRE5MoNocokpyNNJA"]
                                         options:@{}
                               completionHandler:nil];
        return;
    }
    NSArray<UIView *> *views = self.pages[category];
    if (!views)
        return;
    self.userInteractionEnabled = NO;
    [UIView animateWithDuration:0.15 animations:^{
        self.stack.alpha = 0;
        self.stack.transform = CGAffineTransformMakeTranslation(-22, 0);
    } completion:^(__unused BOOL finished) {
        [self clearSettingsStack];
        self.showingRoot = NO;
        self.headerTitle.text = category;
        for (UIView *view in views)
            [self.stack addArrangedSubview:view];
        [self.scrollView setContentOffset:CGPointZero animated:NO];
        self.stack.transform = CGAffineTransformMakeTranslation(22, 0);
        [UIView animateWithDuration:0.4 delay:0
            usingSpringWithDamping:0.86 initialSpringVelocity:0.3 options:UIViewAnimationOptionCurveEaseOut
            animations:^{
                self.stack.alpha = 1;
                self.stack.transform = CGAffineTransformIdentity;
            } completion:^(__unused BOOL complete) { self.userInteractionEnabled = YES; }];
    }];
}

// L1/R1 pad navigation: cycle through the settings categories in the order
// they are listed on the root page.
- (void)padSwitchCategory:(NSInteger)delta {
    NSArray<NSString *> *order = @[
        @"Video", @"Graphics", @"Audio", @"System & Input",
        @"Performance Overlay", @"Library", @"Submit bugs", @"About"
    ];
    if (self.showingRoot) {
        if (delta > 0)
            [self openCategoryNamed:order.firstObject];
        return;
    }
    const NSUInteger current = [order indexOfObject:self.headerTitle.text];
    if (current == NSNotFound)
        return;
    const NSInteger next = (NSInteger)current + delta;
    if (next < 0) {
        [self showSettingsRoot];
        return;
    }
    if (next < (NSInteger)order.count)
        [self openCategoryNamed:order[(NSUInteger)next]];
}

- (void)navigateBack {
    if (self.showingRoot || self.perGameTitleId)
        [self close];
    else
        [self showSettingsRoot];
}

- (void)openDeveloperProfile {
    [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"https://x.com/halcyonpalace"]
                                    options:@{}
                          completionHandler:nil];
}

- (void)showTitleIdsChanged:(UISwitch *)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.on forKey:@"tsubomi.showTitleIds"];
    reload_library_cells();
}

// A defaults-backed switch that reloads the library cells when toggled.
- (UISwitch *)defaultsReloadSwitch:(NSString *)key on:(BOOL)on {
    UISwitch *control = [[UISwitch alloc] init];
    control.on = on;
    control.accessibilityIdentifier = key;
    [control addTarget:self action:@selector(libraryToggleChanged:) forControlEvents:UIControlEventValueChanged];
    return control;
}

- (void)libraryToggleChanged:(UISwitch *)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.on forKey:sender.accessibilityIdentifier];
    reload_library_cells();
}

- (void)openVita3K {
    [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"https://github.com/Vita3K/Vita3K"]
                                    options:@{}
                          completionHandler:nil];
}

- (void)showChangelog {
    present_alert(@"What's new in 0.15.0",
        @"• Graphics settings now include Surface sync globally and per game, which can correct missing or white lighting in Gravity Rush.\n"
        @"• Onboarding titles wrap without clipping, and card dimensions now adapt cleanly when rotating between portrait and landscape.\n"
        @"• The prior sampled-view, graphics-help, bug-submission, settings-animation, and gold-trophy improvements remain included.");
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
    if (sender.on)
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"vita3k.perf.hidden"];
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
    (void)title; // The navigation header already names the active category.
    UIStackView *content = [[UIStackView alloc] init];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 1;
    for (UIView *row in rows)
        [content addArrangedSubview:row];
    // An opaque adaptive card prevents the running game's last frame from
    // bleeding through UIKit material views.
    UIView *card = [[UIView alloc] init];
    card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    card.layer.cornerRadius = 24;
    card.clipsToBounds = YES;
    [card addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14],
        [content.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-14],
        [content.topAnchor constraintEqualToAnchor:card.topAnchor constant:14],
        [content.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-14],
    ]];
    [self.stack addArrangedSubview:card];
}

- (void)resolutionChanged:(UISlider *)slider {
    slider.value = roundf(slider.value * 4.0f) / 4.0f;
    [self updateResolutionLabel];
}

- (void)updateResolutionLabel {
    // %.3g keeps quarter steps exact ("0.75x", "1.25x") where %.2g rounded.
    self.resolutionValue.text = [NSString stringWithFormat:@"%.3gx", self.resolutionSlider.value];
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
    action.settings.fps_limit = 60;
    action.settings.cpu_opt = self.cpuSwitch.on;
    action.settings.ngs_enable = self.ngsSwitch.on;
    action.settings.async_pipeline_compilation = self.asyncSwitch.on;
    action.settings.anisotropic_filtering = anisotropicValues[self.anisotropicControl.selectedSegmentIndex];
    action.settings.high_accuracy = self.highAccuracySwitch.on;
    action.settings.surface_sync = self.surfaceSyncSwitch.on;
    if (self.perGameTitleId) {
        store_game_settings(self.perGameTitleId, action.settings);
        [self close];
        return;
    }
    queue_action(std::move(action));
    [self close];
}

@end


// A pad-navigable stand-in for the cell context menu: real UIButtons in an
// overlay card, so the controller focus system can drive it (UIAlertController
// actions cannot be triggered programmatically).
@interface Vita3KPadMenuView : UIView
@property(nonatomic, strong) NSArray<NSDictionary *> *items;
+ (void)presentInView:(UIView *)host title:(NSString *)title items:(NSArray<NSDictionary *> *)items;
- (void)dismissMenu;
@end

@implementation Vita3KPadMenuView

+ (void)presentInView:(UIView *)host title:(NSString *)title items:(NSArray<NSDictionary *> *)items {
    Vita3KPadMenuView *menu = [[Vita3KPadMenuView alloc] initWithFrame:host.bounds];
    menu.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    menu.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    menu.items = items;
    [menu addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:menu action:@selector(backgroundTapped:)]];

    UIView *card = [[UIView alloc] init];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    card.layer.cornerRadius = 24;
    card.clipsToBounds = YES;
    [menu addSubview:card];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 2;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:stack];

    UILabel *header = [[UILabel alloc] init];
    header.text = title;
    header.font = [UIFont systemFontOfSize:17 weight:UIFontWeightBold];
    header.textColor = UIColor.labelColor;
    header.textAlignment = NSTextAlignmentCenter;
    header.numberOfLines = 2;
    [stack addArrangedSubview:header];
    [stack setCustomSpacing:12 afterView:header];

    NSUInteger index = 0;
    for (NSDictionary *item in items) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.tag = (NSInteger)index++;
        button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
        button.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
        [button setTitle:[NSString stringWithFormat:@"  %@", item[@"title"]] forState:UIControlStateNormal];
        NSString *symbol = item[@"symbol"];
        if (symbol.length)
            [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
        const BOOL destructive = [item[@"destructive"] boolValue];
        button.tintColor = destructive ? UIColor.systemRedColor : UIColor.labelColor;
        [button setTitleColor:destructive ? UIColor.systemRedColor : UIColor.labelColor forState:UIControlStateNormal];
        [button.heightAnchor constraintEqualToConstant:50].active = YES;
        [button addTarget:menu action:@selector(itemTapped:) forControlEvents:UIControlEventTouchUpInside];
        [stack addArrangedSubview:button];
    }
    UIButton *cancel = [UIButton buttonWithType:UIButtonTypeSystem];
    cancel.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    cancel.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [cancel setTitle:@"Cancel" forState:UIControlStateNormal];
    [cancel.heightAnchor constraintEqualToConstant:50].active = YES;
    [cancel addTarget:menu action:@selector(dismissMenu) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:cancel];

    [NSLayoutConstraint activateConstraints:@[
        [card.centerXAnchor constraintEqualToAnchor:menu.centerXAnchor],
        [card.centerYAnchor constraintEqualToAnchor:menu.centerYAnchor],
        [card.widthAnchor constraintEqualToConstant:MIN(360, CGRectGetWidth(host.bounds) - 48)],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18],
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:18],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12],
    ]];

    menu.alpha = 0;
    [host addSubview:menu];
    [UIView animateWithDuration:0.18 animations:^{ menu.alpha = 1; }];
}

- (void)backgroundTapped:(UITapGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateEnded)
        [self dismissMenu];
}

- (void)itemTapped:(UIButton *)sender {
    if (sender.tag >= 0 && sender.tag < (NSInteger)self.items.count) {
        dispatch_block_t handler = self.items[(NSUInteger)sender.tag][@"handler"];
        if (handler)
            handler();
    }
    [self dismissMenu];
}

- (void)dismissMenu {
    [UIView animateWithDuration:0.18 animations:^{ self.alpha = 0; } completion:^(__unused BOOL finished) {
        [self removeFromSuperview];
    }];
}

@end


@interface Vita3KOnboardingView : UIView {
    Vita3KIOSSettings _firmwareSettings;
    Vita3KIOSSettings _importSnapshot;
}
@property(nonatomic) NSInteger pageIndex;
@property(nonatomic, strong) UIImageView *symbolView;
@property(nonatomic, strong) UILabel *titleLabel;
@property(nonatomic, strong) UILabel *bodyLabel;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIButton *primaryButton;
@property(nonatomic, strong) UIButton *nextButton;
@property(nonatomic, strong) UIVisualEffectView *card;
@property(nonatomic, strong) UIStackView *contentStack;
@property(nonatomic, strong) NSLayoutConstraint *cardWidthConstraint;
@property(nonatomic, strong) NSLayoutConstraint *symbolHeightConstraint;
@property(nonatomic, strong) NSLayoutConstraint *primaryHeightConstraint;
@property(nonatomic, strong) NSLayoutConstraint *nextHeightConstraint;
@property(nonatomic, strong) NSLayoutConstraint *contentTopConstraint;
@property(nonatomic, strong) NSLayoutConstraint *contentBottomConstraint;
@property(nonatomic) BOOL usesCompactLandscapeMetrics;
@property(nonatomic) CGFloat lastOnboardingTextWidth;
@property(nonatomic) BOOL hasAnimatedEntrance;
- (instancetype)initWithFrame:(CGRect)frame settings:(const Vita3KIOSSettings &)settings;
- (void)updateFirmwareSettings:(const Vita3KIOSSettings &)settings;
- (void)handleImportResult:(NSString *)message success:(BOOL)success;
@end

@implementation Vita3KOnboardingView

- (instancetype)initWithFrame:(CGRect)frame settings:(const Vita3KIOSSettings &)settings {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundColor = UIColor.systemBackgroundColor;
    _firmwareSettings = settings;
    self.pageIndex = 0;

    self.card = [[UIVisualEffectView alloc] initWithEffect:glass_effect(YES)];
    self.card.translatesAutoresizingMaskIntoConstraints = NO;
    self.card.layer.cornerRadius = 22;
    self.card.clipsToBounds = YES;
    [self addSubview:self.card];

    self.symbolView = [[UIImageView alloc] init];
    self.symbolView.contentMode = UIViewContentModeScaleAspectFit;
    self.symbolView.tintColor = UIColor.systemCyanColor;
    self.symbolHeightConstraint = [self.symbolView.heightAnchor constraintEqualToConstant:52];
    self.symbolHeightConstraint.active = YES;
    self.titleLabel = [[UILabel alloc] init];
    self.titleLabel.font = [UIFont systemFontOfSize:28 weight:UIFontWeightBold];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.titleLabel.numberOfLines = 0;
    self.titleLabel.lineBreakMode = NSLineBreakByWordWrapping;
    self.titleLabel.adjustsFontSizeToFitWidth = YES;
    self.titleLabel.minimumScaleFactor = 0.82;
    self.titleLabel.allowsDefaultTighteningForTruncation = YES;
    [self.titleLabel setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                    forAxis:UILayoutConstraintAxisVertical];
    [self.titleLabel setContentHuggingPriority:UILayoutPriorityRequired
                                       forAxis:UILayoutConstraintAxisVertical];
    self.bodyLabel = [[UILabel alloc] init];
    self.bodyLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightRegular];
    self.bodyLabel.textColor = UIColor.secondaryLabelColor;
    self.bodyLabel.textAlignment = NSTextAlignmentCenter;
    self.bodyLabel.numberOfLines = 0;
    self.bodyLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [self.bodyLabel setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisHorizontal];
    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    self.statusLabel.textColor = UIColor.systemOrangeColor;
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.numberOfLines = 0;
    self.primaryButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.primaryButton.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    self.primaryButton.layer.cornerRadius = 13;
    self.primaryButton.backgroundColor = UIColor.systemCyanColor;
    [self.primaryButton setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
    self.primaryHeightConstraint = [self.primaryButton.heightAnchor constraintEqualToConstant:44];
    self.primaryHeightConstraint.active = YES;
    [self.primaryButton addTarget:self action:@selector(primaryPressed) forControlEvents:UIControlEventTouchUpInside];
    self.nextButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.nextButton.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [self.nextButton setTitle:@"Next" forState:UIControlStateNormal];
    self.nextHeightConstraint = [self.nextButton.heightAnchor constraintEqualToConstant:42];
    self.nextHeightConstraint.active = YES;
    [self.nextButton addTarget:self action:@selector(nextPressed) forControlEvents:UIControlEventTouchUpInside];

    self.contentStack = [[UIStackView alloc] initWithArrangedSubviews:@[
        self.symbolView, self.titleLabel, self.bodyLabel, self.statusLabel,
        self.primaryButton, self.nextButton]];
    self.contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.contentStack.axis = UILayoutConstraintAxisVertical;
    self.contentStack.alignment = UIStackViewAlignmentFill;
    self.contentStack.spacing = 10;
    [self.card.contentView addSubview:self.contentStack];
    self.cardWidthConstraint = [self.card.widthAnchor constraintEqualToConstant:320];
    self.contentTopConstraint = [self.contentStack.topAnchor constraintEqualToAnchor:self.card.contentView.topAnchor constant:18];
    self.contentBottomConstraint = [self.contentStack.bottomAnchor constraintEqualToAnchor:self.card.contentView.bottomAnchor constant:-18];
    [NSLayoutConstraint activateConstraints:@[
        [self.card.centerXAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerXAnchor],
        [self.card.centerYAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerYAnchor],
        [self.card.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor constant:24],
        [self.card.trailingAnchor constraintLessThanOrEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor constant:-24],
        self.cardWidthConstraint,
        [self.contentStack.leadingAnchor constraintEqualToAnchor:self.card.contentView.leadingAnchor constant:20],
        [self.contentStack.trailingAnchor constraintEqualToAnchor:self.card.contentView.trailingAnchor constant:-20],
        self.contentTopConstraint,
        self.contentBottomConstraint,
    ]];
    [self renderPageAnimated:NO];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.backgroundColor = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark
        ? UIColor.blackColor : UIColor.whiteColor;
    const CGRect safeFrame = self.safeAreaLayoutGuide.layoutFrame;
    if (CGRectIsEmpty(safeFrame))
        return;
    const CGFloat safeWidth = CGRectGetWidth(safeFrame);
    const CGFloat safeHeight = CGRectGetHeight(safeFrame);
    const BOOL landscape = safeWidth > safeHeight;
    const CGFloat horizontalSpace = landscape ? 96 : 48;
    const CGFloat maximumWidth = landscape ? 400 : 360;
    const CGFloat availableWidth = MAX(1, safeWidth - 48);
    const CGFloat targetWidth = MIN(availableWidth,
        MAX(240, MIN(maximumWidth, safeWidth - horizontalSpace)));
    const BOOL widthChanged = fabs(self.cardWidthConstraint.constant - targetWidth) > 0.5;
    const BOOL metricsChanged = self.usesCompactLandscapeMetrics != landscape;
    if (widthChanged)
        self.cardWidthConstraint.constant = targetWidth;

    if (metricsChanged) {
        self.usesCompactLandscapeMetrics = landscape;
        self.contentStack.spacing = landscape ? 6 : 10;
        self.symbolHeightConstraint.constant = landscape ? 40 : 52;
        self.primaryHeightConstraint.constant = landscape ? 38 : 44;
        self.nextHeightConstraint.constant = landscape ? 38 : 42;
        self.contentTopConstraint.constant = landscape ? 12 : 18;
        self.contentBottomConstraint.constant = landscape ? -12 : -18;
        self.titleLabel.font = [UIFont systemFontOfSize:(landscape ? 24 : 28) weight:UIFontWeightBold];
        self.bodyLabel.font = [UIFont systemFontOfSize:(landscape ? 13 : 15) weight:UIFontWeightRegular];
    }

    const CGFloat textWidth = MAX(1, targetWidth - 40);
    if (metricsChanged || fabs(self.lastOnboardingTextWidth - textWidth) > 0.5) {
        self.lastOnboardingTextWidth = textWidth;
        self.titleLabel.preferredMaxLayoutWidth = textWidth;
        self.bodyLabel.preferredMaxLayoutWidth = textWidth;
        self.statusLabel.preferredMaxLayoutWidth = textWidth;
        [self.titleLabel invalidateIntrinsicContentSize];
        [self.bodyLabel invalidateIntrinsicContentSize];
        [self.statusLabel invalidateIntrinsicContentSize];
    }
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (!self.window || self.hasAnimatedEntrance)
        return;
    self.hasAnimatedEntrance = YES;
    self.card.alpha = 0;
    self.card.transform = CGAffineTransformMakeScale(0.94, 0.94);
    [UIView animateWithDuration:0.48 delay:0.04
        usingSpringWithDamping:0.84 initialSpringVelocity:0.25 options:UIViewAnimationOptionCurveEaseOut
        animations:^{
            self.card.alpha = 1;
            self.card.transform = CGAffineTransformIdentity;
        } completion:nil];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    [super traitCollectionDidChange:previousTraitCollection];
    if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle)
        [self setNeedsLayout];
}

- (BOOL)requiredPackageReady {
    if (self.pageIndex == 2)
        return _firmwareSettings.preinstalled_package_ready;
    if (self.pageIndex == 3)
        return _firmwareSettings.font_package_ready;
    if (self.pageIndex == 4)
        return _firmwareSettings.main_firmware_ready;
    return YES;
}

- (void)renderPageAnimated:(BOOL)animated {
    NSArray<NSString *> *symbols = @[@"sparkles", @"checkmark.shield", @"shippingbox",
        @"textformat", @"gearshape.2", @"flask"];
    NSArray<NSString *> *titles = @[@"Welcome to Tsubomi", @"Bring Your Own Games",
        @"Install PREINSTALL.PUP", @"Install FONTPKG.PUP", @"Install PSVUPDAT.PUP",
        @"Experimental Software"];
    NSArray<NSString *> *bodies = @[
        @"",
        @"Piracy is not supported. You must supply your own legally obtained game dumps and license files; Tsubomi does not include games, firmware, keys, or licenses.",
        @"Choose the official PREINSTALL.PUP from your own Vita firmware files. This installs the preinstalled system content required by games.",
        @"Choose the official FONTPKG.PUP. This installs the Vita system fonts used by games and the emulator.",
        @"Choose the official PSVUPDAT.PUP. This installs the main Vita system firmware.",
        @"Not every game works yet. Expect graphics glitches, crashes, missing features, and performance issues. Please keep useful logs when something breaks."
    ];
    void (^changes)(void) = ^{
        self.symbolView.image = [UIImage systemImageNamed:symbols[self.pageIndex]];
        self.titleLabel.text = titles[self.pageIndex];
        self.bodyLabel.text = bodies[self.pageIndex];
        [self.titleLabel invalidateIntrinsicContentSize];
        [self.bodyLabel invalidateIntrinsicContentSize];
        self.bodyLabel.hidden = self.pageIndex == 0;
        self.symbolView.hidden = self.pageIndex == 0;
        self.statusLabel.text = @"";
        const BOOL firmwarePage = self.pageIndex >= 2 && self.pageIndex <= 4;
        self.primaryButton.hidden = !firmwarePage && self.pageIndex != 5;
        self.nextButton.hidden = self.pageIndex == 5;
        if (firmwarePage) {
            [self.primaryButton setTitle:@"Choose PUP" forState:UIControlStateNormal];
            self.nextButton.enabled = [self requiredPackageReady];
            self.nextButton.alpha = self.nextButton.enabled ? 1 : 0.35;
            if (self.nextButton.enabled) {
                self.statusLabel.text = @"Installed ✓";
                self.statusLabel.textColor = UIColor.systemGreenColor;
            }
        } else if (self.pageIndex == 5) {
            [self.primaryButton setTitle:@"Get Started" forState:UIControlStateNormal];
            self.primaryButton.enabled = _firmwareSettings.firmware_ready;
        } else {
            self.nextButton.enabled = YES;
            self.nextButton.alpha = 1;
        }
    };
    if (animated) {
        [UIView animateWithDuration:0.16 animations:^{
            self.contentStack.alpha = 0;
            self.contentStack.transform = CGAffineTransformMakeTranslation(-24, 0);
        } completion:^(__unused BOOL finished) {
            changes();
            self.contentStack.transform = CGAffineTransformMakeTranslation(24, 0);
            [UIView animateWithDuration:0.42 delay:0
                usingSpringWithDamping:0.84 initialSpringVelocity:0.35 options:UIViewAnimationOptionCurveEaseOut
                animations:^{
                    self.contentStack.alpha = 1;
                    self.contentStack.transform = CGAffineTransformIdentity;
                } completion:nil];
        }];
    } else {
        changes();
    }
}

- (void)primaryPressed {
    if (self.pageIndex >= 2 && self.pageIndex <= 4) {
        _importSnapshot = _firmwareSettings;
        present_import_picker(YES);
        return;
    }
    if (self.pageIndex == 5 && _firmwareSettings.firmware_ready) {
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"tsubomi.onboarded"];
        [UIView animateWithDuration:0.35 animations:^{ self.alpha = 0; }
            completion:^(__unused BOOL finished) { [self removeFromSuperview]; }];
    }
}

- (void)nextPressed {
    if ((self.pageIndex >= 2 && self.pageIndex <= 4) && ![self requiredPackageReady])
        return;
    if (self.pageIndex < 5) {
        ++self.pageIndex;
        [self renderPageAnimated:YES];
    }
}

- (void)updateFirmwareSettings:(const Vita3KIOSSettings &)settings {
    _firmwareSettings = settings;
    if (self.pageIndex >= 2 && self.pageIndex <= 4)
        [self renderPageAnimated:NO];
}

- (void)handleImportResult:(NSString *)message success:(BOOL)success {
    if (self.pageIndex < 2 || self.pageIndex > 4)
        return;
    if (!success) {
        self.statusLabel.text = message;
        self.statusLabel.textColor = UIColor.systemRedColor;
        return;
    }
    if ([self requiredPackageReady]) {
        self.statusLabel.text = @"Installed successfully. Tap Next to continue.";
        self.statusLabel.textColor = UIColor.systemGreenColor;
        self.nextButton.enabled = YES;
        self.nextButton.alpha = 1;
        return;
    }
    NSString *installed = nil;
    if (!_importSnapshot.preinstalled_package_ready && _firmwareSettings.preinstalled_package_ready)
        installed = @"PREINSTALL.PUP";
    else if (!_importSnapshot.font_package_ready && _firmwareSettings.font_package_ready)
        installed = @"FONTPKG.PUP";
    else if (!_importSnapshot.main_firmware_ready && _firmwareSettings.main_firmware_ready)
        installed = @"PSVUPDAT.PUP";
    self.statusLabel.text = installed
        ? [NSString stringWithFormat:@"You selected %@. This page needs %@; please choose that file.", installed,
            self.pageIndex == 2 ? @"PREINSTALL.PUP" : (self.pageIndex == 3 ? @"FONTPKG.PUP" : @"PSVUPDAT.PUP")]
        : message;
    self.statusLabel.textColor = UIColor.systemOrangeColor;
}

@end


@interface Vita3KLibraryView : UIView <UICollectionViewDataSource, UICollectionViewDelegateFlowLayout> {
@public
    std::vector<Vita3KIOSGameEntry> _games;
    Vita3KIOSSettings _settings;
    BOOL _jitAvailable;
    BOOL _listMode;
    // Landscape card view is a centered, infinitely-looping cover carousel.
    BOOL _carouselActive;
    BOOL _carouselNeedsCentering;
    CGFloat _lastCollectionWidth;
}
@property(nonatomic, strong) UICollectionView *collectionView;
@property(nonatomic, strong) UILabel *emptyLabel;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIVisualEffectView *statusGlass;
@property(nonatomic, strong) UIView *busyOverlay;
@property(nonatomic, strong) UIVisualEffectView *headerGlass;
@property(nonatomic, strong) CAGradientLayer *backgroundGradient;
@property(nonatomic, strong) UIVisualEffectView *firmwareGlass;
@property(nonatomic, strong) UILabel *firmwareLabel;
@property(nonatomic, strong) UIVisualEffectView *jitBanner;
@property(nonatomic, strong) UILabel *jitBannerLabel;
@property(nonatomic, strong) Vita3KOnboardingView *onboardingView;
- (void)updateGames:(const std::vector<Vita3KIOSGameEntry> &)games settings:(const Vita3KIOSSettings &)settings;
- (void)setJitAvailable:(BOOL)available;
- (void)presentPadActionsForItem:(NSInteger)item;
- (BOOL)jitAvailable;
- (BOOL)firmwareReadyOrPresentAlert;
@end

@implementation Vita3KLibraryView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    _jitAvailable = YES;
    // List view is the default presentation (key absent = list).
    NSUserDefaults *viewDefaults = NSUserDefaults.standardUserDefaults;
    _listMode = [viewDefaults objectForKey:@"tsubomi.libraryListMode"]
        ? [viewDefaults boolForKey:@"tsubomi.libraryListMode"]
        : YES;
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
    UIButton *help = symbol_button(@"questionmark.circle", @"Help", @"Graphics help");
    help.tag = 106;
    [help addTarget:self action:@selector(showHelp) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:help];
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

    // Firmware version indicator: a plain label (no glass material).
    self.firmwareGlass = [[UIVisualEffectView alloc] initWithEffect:nil];
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
    [self.collectionView registerClass:UICollectionReusableView.class
        forSupplementaryViewOfKind:UICollectionElementKindSectionFooter withReuseIdentifier:@"totals"];
    [self insertSubview:self.collectionView belowSubview:self.headerGlass];

    self.emptyLabel = [[UILabel alloc] init];
    self.emptyLabel.text = @"No games yet\n\nTap + to import a game (.vpk/.zip/.pkg), or copy\nPC's Vita3K data into Documents/Tsubomi/vita";
    self.emptyLabel.textColor = UIColor.secondaryLabelColor;
    self.emptyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    self.emptyLabel.textAlignment = NSTextAlignmentCenter;
    self.emptyLabel.numberOfLines = 0;
    [self addSubview:self.emptyLabel];

    // Transient toast ("Settings saved", "Game imported", …) in a glass
    // capsule so the orange text stays readable over library content.
    self.statusGlass = [[UIVisualEffectView alloc] initWithEffect:glass_effect(NO)];
    self.statusGlass.layer.cornerRadius = 14;
    self.statusGlass.clipsToBounds = YES;
    self.statusGlass.alpha = 0;
    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.textColor = UIColor.systemOrangeColor;
    self.statusLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.numberOfLines = 2;
    [self.statusGlass.contentView addSubview:self.statusLabel];
    [self addSubview:self.statusGlass];
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
    UIButton *help = [self viewWithTag:106];
    const CGFloat usableWidth = CGRectGetWidth(self.bounds) - safe.left - safe.right;
    const BOOL compactHeader = usableWidth < 600;
    // Single navigation-bar-style row: large title on the left, plain glyph
    // controls right-aligned on the same baseline.
    const CGFloat rowY = safe.top + 8;
    const CGFloat rowHeight = 44;
    const CGFloat buttonSize = 40;
    const CGFloat buttonY = rowY + (rowHeight - buttonSize) / 2;
    settings.frame = CGRectMake(CGRectGetWidth(self.bounds) - safe.right - 14 - buttonSize, buttonY, buttonSize, buttonSize);
    help.frame = CGRectMake(CGRectGetMinX(settings.frame) - buttonSize - 2, buttonY, buttonSize, buttonSize);
    refresh.frame = CGRectMake(CGRectGetMinX(help.frame) - buttonSize - 2, buttonY, buttonSize, buttonSize);
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
    // The status toast floats over the content edge; it must not reserve
    // permanent header height. Size the capsule to its text.
    const CGFloat statusMaxWidth = MAX(1, usableWidth - 40);
    const CGSize statusText = [self.statusLabel sizeThatFits:CGSizeMake(statusMaxWidth - 28, CGFLOAT_MAX)];
    const CGFloat statusWidth = MIN(statusMaxWidth, statusText.width + 28);
    const CGFloat statusHeight = MAX(28, statusText.height + 12);
    self.statusGlass.frame = CGRectMake(safe.left + (usableWidth - statusWidth) / 2,
        headerBottom + 5, statusWidth, statusHeight);
    self.statusLabel.frame = self.statusGlass.bounds;
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
    // In carousel mode syncCarouselMode owns the insets; writing the grid
    // insets first made every layout pass ping-pong them and stutter touch
    // scrolling.
    if (![self carouselShouldBeActive])
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
    [self syncCarouselMode:contentTop];
    [self updateHeaderGlassVisibility];
}

// ---- Landscape cover carousel ----------------------------------------------

// Loop the games list many times so scrolling feels endless in both
// directions; indexes map back with modulo.
static const NSInteger kCarouselRepeat = 400;

- (BOOL)carouselShouldBeActive {
    return !_listMode && !_games.empty()
        && CGRectGetWidth(self.bounds) > CGRectGetHeight(self.bounds);
}

- (CGFloat)carouselCoverSide {
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat availableHeight = CGRectGetHeight(self.bounds) - safe.top - safe.bottom - 170;
    return MAX(120, MIN(availableHeight, CGRectGetWidth(self.bounds) * 0.34));
}

- (void)syncCarouselMode:(CGFloat)contentTop {
    const BOOL shouldBeActive = [self carouselShouldBeActive];
    UICollectionViewFlowLayout *layout = (UICollectionViewFlowLayout *)self.collectionView.collectionViewLayout;
    if (shouldBeActive != _carouselActive) {
        _carouselActive = shouldBeActive;
        layout.scrollDirection = _carouselActive
            ? UICollectionViewScrollDirectionHorizontal
            : UICollectionViewScrollDirectionVertical;
        // The carousel scrolls on one axis only; vertical bounce let the whole
        // row drag up and down.
        self.collectionView.alwaysBounceVertical = !_carouselActive;
        self.collectionView.alwaysBounceHorizontal = _carouselActive;
        self.collectionView.showsHorizontalScrollIndicator = NO;
        self.collectionView.decelerationRate = _carouselActive
            ? UIScrollViewDecelerationRateFast
            : UIScrollViewDecelerationRateNormal;
        _carouselNeedsCentering = _carouselActive;
        [layout invalidateLayout];
        [self.collectionView reloadData];
    }
    if (_carouselActive) {
        const CGFloat side = [self carouselCoverSide];
        const UIEdgeInsets safe = self.safeAreaInsets;
        // Center vertically in the space under the header; side insets center
        // the focused cover horizontally.
        const CGFloat itemHeight = side + 60;
        const CGFloat verticalSpace = CGRectGetHeight(self.bounds) - contentTop - safe.bottom;
        const CGFloat topInset = contentTop + MAX(0, (verticalSpace - itemHeight) / 2);
        const CGFloat horizontalInset = MAX(0, (CGRectGetWidth(self.bounds) - side) / 2);
        const UIEdgeInsets desired = UIEdgeInsetsMake(topInset, horizontalInset, safe.bottom, horizontalInset);
        // Re-setting an identical inset mid-gesture stutters the scroll.
        if (!UIEdgeInsetsEqualToEdgeInsets(self.collectionView.contentInset, desired))
            self.collectionView.contentInset = desired;
        if (_carouselNeedsCentering) {
            _carouselNeedsCentering = NO;
            [self.collectionView layoutIfNeeded];
            const NSInteger middle = (kCarouselRepeat / 2) * static_cast<NSInteger>(_games.size());
            [self.collectionView scrollToItemAtIndexPath:[NSIndexPath indexPathForItem:middle inSection:0]
                                        atScrollPosition:UICollectionViewScrollPositionCenteredHorizontally
                                                animated:NO];
            [self applyCarouselTransforms];
        }
    }
}

- (CGFloat)carouselStride {
    UICollectionViewFlowLayout *layout = (UICollectionViewFlowLayout *)self.collectionView.collectionViewLayout;
    return [self carouselCoverSide] + layout.minimumLineSpacing;
}

// Scale and dim covers by their distance from the horizontal center, the way
// a music-library shuffle presents the focused album.
- (void)applyCarouselTransforms {
    if (!_carouselActive)
        return;
    const CGFloat centerX = self.collectionView.contentOffset.x + CGRectGetWidth(self.collectionView.bounds) / 2;
    for (UICollectionViewCell *cell in self.collectionView.visibleCells) {
        const CGFloat distance = fabs(cell.center.x - centerX) / MAX(1, [self carouselStride]);
        const CGFloat closeness = MAX(0.0, 1.0 - MIN(distance, 1.0));
        const CGFloat scale = 0.78 + 0.22 * closeness;
        cell.transform = CGAffineTransformMakeScale(scale, scale);
        cell.alpha = 0.5 + 0.5 * closeness;
    }
}

// Reused cells enter the screen untransformed for one frame otherwise — the
// visible "flicker" while swiping the carousel by touch.
- (void)collectionView:(UICollectionView *)collectionView
       willDisplayCell:(UICollectionViewCell *)cell
    forItemAtIndexPath:(NSIndexPath *)indexPath {
    (void)collectionView;
    (void)indexPath;
    if (!_carouselActive)
        return;
    const CGFloat centerX = self.collectionView.contentOffset.x + CGRectGetWidth(self.collectionView.bounds) / 2;
    const CGFloat distance = fabs(cell.center.x - centerX) / MAX(1, [self carouselStride]);
    const CGFloat closeness = MAX(0.0, 1.0 - MIN(distance, 1.0));
    const CGFloat scale = 0.78 + 0.22 * closeness;
    cell.transform = CGAffineTransformMakeScale(scale, scale);
    cell.alpha = 0.5 + 0.5 * closeness;
}

- (void)scrollViewWillEndDragging:(UIScrollView *)scrollView
                     withVelocity:(CGPoint)velocity
              targetContentOffset:(inout CGPoint *)targetContentOffset {
    if (!_carouselActive || scrollView != self.collectionView)
        return;
    // Snap so a cover always rests centered.
    const CGFloat stride = [self carouselStride];
    const CGFloat base = -scrollView.contentInset.left;
    CGFloat target = targetContentOffset->x;
    // Nudge in the fling direction so gentle flicks advance one cover.
    if (fabs(velocity.x) > 0.2)
        target += stride * 0.5 * (velocity.x > 0 ? 1 : -1);
    const CGFloat snapped = base + round((target - base) / stride) * stride;
    targetContentOffset->x = snapped;
}

// On rotation the safe areas settle after the first layout pass, so insets
// computed during it are stale (content under the notch one way, centered the
// other). Re-run layout and re-ask for cell sizes when they change.
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsLayout];
    [self.collectionView.collectionViewLayout invalidateLayout];
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
    if (scrollView != self.collectionView)
        return;
    if (_carouselActive)
        [self applyCarouselTransforms];
    else
        [self updateHeaderGlassVisibility];
}

// Map a (possibly looped) collection item back to its game index.
- (NSInteger)gameIndexForItem:(NSInteger)item {
    if (_games.empty())
        return 0;
    return item % static_cast<NSInteger>(_games.size());
}

- (void)toggleViewMode:(UIButton *)sender {
    _listMode = !_listMode;
    [NSUserDefaults.standardUserDefaults setBool:_listMode forKey:@"tsubomi.libraryListMode"];
    [sender setImage:[UIImage systemImageNamed:_listMode ? @"square.grid.2x2" : @"list.bullet"]
            forState:UIControlStateNormal];
    [self.collectionView.collectionViewLayout invalidateLayout];
    [self.collectionView reloadData];
    [self setNeedsLayout]; // re-evaluate the landscape carousel mode
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
    [self.onboardingView updateFirmwareSettings:settings];
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
    const auto count = static_cast<NSInteger>(_games.size());
    return _carouselActive ? count * kCarouselRepeat : count;
}

- (__kindof UICollectionViewCell *)collectionView:(UICollectionView *)collectionView cellForItemAtIndexPath:(NSIndexPath *)indexPath {
    Vita3KGameCell *cell = [collectionView dequeueReusableCellWithReuseIdentifier:@"game" forIndexPath:indexPath];
    cell.carouselMode = _carouselActive;
    const auto &game = _games.at(static_cast<std::size_t>([self gameIndexForItem:indexPath.item]));
    NSString *identifier = [NSString stringWithUTF8String:game.title_id.c_str()] ?: @"Unknown title ID";
    NSString *title = [NSString stringWithUTF8String:game.title.c_str()];
    if (!title) {
        LOG_ERROR("iOS library title is invalid UTF-8: title_id={} bytes={}", game.title_id, hex_bytes(game.title));
        title = [NSString stringWithFormat:@"Unknown title (%@)", identifier];
    }
    NSString *iconPath = [NSString stringWithUTF8String:game.icon_path.c_str()];
    if (has_custom_cover(identifier))
        iconPath = cover_render_path(identifier);
    NSAttributedString *metadata;
    if (_carouselActive) {
        // Compact one-liner under the focused cover.
        metadata = [[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"%@  ·  %@", played_time_text(game), last_played_text(game)]
                attributes:@{
                    NSFontAttributeName: [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1],
                    NSForegroundColorAttributeName: UIColor.secondaryLabelColor,
                }];
    } else {
        metadata = game_metadata(game);
    }
    [cell configureTitle:display_title(identifier, title) identifier:identifier metadata:metadata
                  iconPath:iconPath listMode:_listMode];
    if (!_carouselActive)
        cell.alpha = _settings.firmware_ready ? 1.0 : 0.55;
    return cell;
}

// Deleting removes the installed content (app/patch/DLC). Saves, licenses,
// and trophy progress stay so a reinstall picks them back up.
- (void)confirmDeleteGame:(NSString *)identifier title:(NSString *)title {
    UIViewController *root = active_window().rootViewController;
    if (!root)
        return;
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:[NSString stringWithFormat:@"Delete %@?", title]
                         message:@"The installed game, its update, and DLC are removed from this device. Saves and trophies are kept."
                  preferredStyle:UIAlertControllerStyleAlert];
    __weak Vita3KLibraryView *weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive
        handler:^(__unused UIAlertAction *action) {
            [weakSelf showBusyOverlay:@"Deleting game…" blockInteraction:YES];
            Vita3KIOSFrontendAction request;
            request.kind = Vita3KIOSFrontendActionKind::DeleteGame;
            request.title_id = identifier.UTF8String;
            queue_action(std::move(request));
        }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [root presentViewController:alert animated:YES completion:nil];
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
    if ([self gameIndexForItem:indexPath.item] >= static_cast<NSInteger>(_games.size()))
        return nil;
    const auto &game = _games.at(static_cast<std::size_t>([self gameIndexForItem:indexPath.item]));
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
            UIAction *gameSettings = [UIAction actionWithTitle:@"Game settings"
                image:[UIImage systemImageNamed:@"slider.horizontal.3"]
                identifier:nil handler:^(__unused UIAction *action) { [weakSelf presentGameSettings:identifier]; }];
            UIAction *coverArt = [UIAction actionWithTitle:@"Custom cover art"
                image:[UIImage systemImageNamed:@"photo"]
                identifier:nil handler:^(__unused UIAction *action) { present_cover_picker(identifier); }];
            NSMutableArray<UIMenuElement *> *children = [NSMutableArray arrayWithObjects:
                importSave, exportSave, rename, trophies, gameSettings, coverArt, nil];
            if (has_game_settings(identifier)) {
                UIAction *globalSettings = [UIAction actionWithTitle:@"Use global settings"
                    image:[UIImage systemImageNamed:@"arrow.uturn.backward.circle"]
                    identifier:nil
                    handler:^(__unused UIAction *action) {
                        [NSUserDefaults.standardUserDefaults removeObjectForKey:game_settings_key(identifier)];
                    }];
                [children addObject:globalSettings];
            }
            // Crop works on the picked photo when one exists, otherwise on the
            // game's default art — so the built-in cover can be reframed too.
            NSString *defaultArtPath = [NSString stringWithUTF8String:game.icon_path.c_str()] ?: @"";
            UIAction *adjustCrop = [UIAction actionWithTitle:@"Adjust cover crop"
                image:[UIImage systemImageNamed:@"crop"]
                identifier:nil
                handler:^(__unused UIAction *action) {
                    UIImage *source = [UIImage imageWithContentsOfFile:cover_original_path(identifier)]
                        ?: [UIImage imageWithContentsOfFile:defaultArtPath];
                    present_cover_crop(identifier, source);
                }];
            [children addObject:adjustCrop];
            UIAction *deleteGame = [UIAction actionWithTitle:@"Delete game"
                image:[UIImage systemImageNamed:@"trash"]
                identifier:nil
                handler:^(__unused UIAction *action) { [weakSelf confirmDeleteGame:identifier title:original]; }];
            deleteGame.attributes = UIMenuElementAttributesDestructive;
            [children addObject:deleteGame];
            if (has_custom_cover(identifier)) {
                UIAction *resetCover = [UIAction actionWithTitle:@"Reset cover art"
                    image:[UIImage systemImageNamed:@"photo.badge.arrow.down"]
                    identifier:nil
                    handler:^(__unused UIAction *action) {
                        [NSFileManager.defaultManager removeItemAtPath:cover_render_path(identifier) error:nil];
                        [NSFileManager.defaultManager removeItemAtPath:cover_original_path(identifier) error:nil];
                        [weakSelf.collectionView reloadData];
                    }];
                resetCover.attributes = UIMenuElementAttributesDestructive;
                [children addObject:resetCover];
            }
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
    // Derive the usable width from the CURRENT safe areas rather than the
    // collection view's contentInset: during rotation this method can run
    // before layoutSubviews has pushed the new insets, and stale insets are
    // exactly the too-wide/too-narrow cell bugs seen on orientation change.
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat usable = CGRectGetWidth(self.bounds) - safe.left - safe.right;
    const CGFloat collectionInset = usable < 600 ? 10 : 18;
    const CGFloat width = usable - collectionInset * 2;
    if (_carouselActive) {
        const CGFloat side = [self carouselCoverSide];
        return CGSizeMake(side, side + 60);
    }
    if (_listMode)
        return CGSizeMake(floor(width), library_list_row_height());
    const CGFloat spacing = 14;
    // Choose the column count from a larger target cell width (~220pt) rather than a
    // couple of fixed width thresholds. Phone landscape (~750-800pt of grid)
    // then packs 4 sensible cells instead of 3 ballooned ones, while portrait
    // still lands on 2 columns.
    const CGFloat targetItemWidth = 220;
    const NSInteger columns = MAX(2, static_cast<NSInteger>(floor((width + spacing) / (targetItemWidth + spacing))));
    const CGFloat itemWidth = floor((width - (columns - 1) * spacing) / columns);
    return CGSizeMake(itemWidth, itemWidth + library_card_label_height() + 4);
}

// List view ends with the library's total footprint.
- (CGSize)collectionView:(UICollectionView *)collectionView layout:(UICollectionViewLayout *)layout
    referenceSizeForFooterInSection:(NSInteger)section {
    (void)collectionView;
    (void)layout;
    (void)section;
    return _listMode && !_games.empty() ? CGSizeMake(1, 44) : CGSizeZero;
}

- (UICollectionReusableView *)collectionView:(UICollectionView *)collectionView
    viewForSupplementaryElementOfKind:(NSString *)kind atIndexPath:(NSIndexPath *)indexPath {
    UICollectionReusableView *footer = [collectionView dequeueReusableSupplementaryViewOfKind:kind
        withReuseIdentifier:@"totals" forIndexPath:indexPath];
    UILabel *label = [footer viewWithTag:301];
    if (!label) {
        label = [[UILabel alloc] init];
        label.tag = 301;
        label.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
        label.textColor = UIColor.secondaryLabelColor;
        label.textAlignment = NSTextAlignmentCenter;
        label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        label.frame = footer.bounds;
        [footer addSubview:label];
    }
    std::uint64_t total_bytes = 0;
    for (const auto &game : _games)
        total_bytes += game.size_bytes;
    NSByteCountFormatter *bytes = [[NSByteCountFormatter alloc] init];
    bytes.countStyle = NSByteCountFormatterCountStyleFile;
    label.text = [NSString stringWithFormat:@"%zu game%s · %@ total",
        _games.size(), _games.size() == 1 ? "" : "s",
        [bytes stringFromByteCount:(long long)total_bytes]];
    return footer;
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
    const auto &game = _games.at(static_cast<std::size_t>([self gameIndexForItem:indexPath.item]));
    [self showBootingOverlay:[NSString stringWithUTF8String:game.title.c_str()] ?: @"game"];
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::Launch;
    action.app_path = game.app_path;
    NSString *identifier = [NSString stringWithUTF8String:game.title_id.c_str()] ?: @"";
    if (has_game_settings(identifier)) {
        action.settings = game_settings_or(identifier, _settings);
        action.has_settings_override = true;
    }
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

- (void)showHelp {
    present_alert(@"Graphics help",
        @"If a game's shaders or textures do not look correct, open that game's settings and try Graphics > High accuracy. If lighting appears white or missing, also enable Graphics > Surface sync.");
}

- (void)settings {
    Vita3KSettingsView *settings = [[Vita3KSettingsView alloc] initWithFrame:self.bounds values:_settings];
    settings.alpha = 0;
    [self addSubview:settings];
    // Belt and braces: a lingering game drawable must never be visible under
    // the settings page.
    set_metal_drawables_hidden(self.window, YES);
    [UIView animateWithDuration:0.22 animations:^{ settings.alpha = 1; }];
}

// Per-game settings: the same emulator controls, saved as an override for one
// title and applied only when that title boots.
- (void)presentGameSettings:(NSString *)identifier {
    Vita3KSettingsView *settings = [[Vita3KSettingsView alloc]
        initWithFrame:self.bounds values:game_settings_or(identifier, _settings)];
    [settings enterPerGameModeForTitle:identifier];
    settings.alpha = 0;
    [self addSubview:settings];
    [UIView animateWithDuration:0.22 animations:^{ settings.alpha = 1; }];
}

// Triangle on a focused cell: the same actions as the long-press context menu,
// in a pad-navigable overlay.
- (void)presentPadActionsForItem:(NSInteger)item {
    if (item < 0)
        return;
    item = [self gameIndexForItem:item];
    if (item >= static_cast<NSInteger>(_games.size()))
        return;
    const auto &game = _games.at(static_cast<std::size_t>(item));
    NSString *identifier = [NSString stringWithUTF8String:game.title_id.c_str()] ?: @"";
    NSString *original = [NSString stringWithUTF8String:game.title.c_str()] ?: identifier;
    NSString *trophyId = [NSString stringWithUTF8String:game.trophy_id.c_str()] ?: @"";
    __weak Vita3KLibraryView *weakSelf = self;
    NSMutableArray<NSDictionary *> *items = [NSMutableArray array];
    [items addObject:@{ @"title": @"Import save", @"symbol": @"square.and.arrow.down",
        @"handler": [^{ present_save_picker(identifier); } copy] }];
    [items addObject:@{ @"title": @"Export save", @"symbol": @"square.and.arrow.up",
        @"handler": [^{
            [weakSelf showBusyOverlay:@"Exporting save…" blockInteraction:YES];
            Vita3KIOSFrontendAction request;
            request.kind = Vita3KIOSFrontendActionKind::ExportSave;
            request.title_id = identifier.UTF8String;
            queue_action(std::move(request));
        } copy] }];
    [items addObject:@{ @"title": @"Rename title", @"symbol": @"pencil",
        @"handler": [^{ [weakSelf promptRename:identifier original:original]; } copy] }];
    [items addObject:@{ @"title": @"Game settings", @"symbol": @"slider.horizontal.3",
        @"handler": [^{ [weakSelf presentGameSettings:identifier]; } copy] }];
    [items addObject:@{ @"title": @"Custom cover art", @"symbol": @"photo",
        @"handler": [^{ present_cover_picker(identifier); } copy] }];
    [items addObject:@{ @"title": @"View trophies", @"symbol": @"trophy.fill",
        @"handler": [^{
            Vita3KIOSFrontendAction request;
            request.kind = Vita3KIOSFrontendActionKind::ShowTrophies;
            request.title_id = original.UTF8String;
            request.app_path = identifier.UTF8String;
            request.trophy_id = trophyId.UTF8String;
            queue_action(std::move(request));
        } copy] }];
    if ([NSUserDefaults.standardUserDefaults stringForKey:title_override_key(identifier)].length) {
        [items addObject:@{ @"title": @"Reset name", @"symbol": @"arrow.uturn.backward", @"destructive": @YES,
            @"handler": [^{
                [NSUserDefaults.standardUserDefaults removeObjectForKey:title_override_key(identifier)];
                [weakSelf.collectionView reloadData];
            } copy] }];
    }
    [Vita3KPadMenuView presentInView:self title:display_title(identifier, original) items:items];
}

- (void)reportRestartRequired:(NSArray<NSString *> *)settings {
    self.statusLabel.text = settings.count
        ? [NSString stringWithFormat:@"Saved · restart required: %@", [settings componentsJoinedByString:@", "]]
        : @"Settings saved and applied";
    [self setNeedsLayout];
    [self layoutIfNeeded];
    self.statusGlass.alpha = 1;
    [UIView animateWithDuration:0.3 delay:4 options:0 animations:^{ self.statusGlass.alpha = 0; } completion:nil];
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


// Spatial controller navigation for the native UI (library, settings, pad
// menus): DPAD moves a focus ring, Cross activates, Circle goes back, Triangle
// opens the focused game's actions, L1/R1 switch settings categories. Handlers
// no-op whenever the library UI is not on screen, so in-game input is never
// intercepted.
@interface Vita3KPadNavigator : NSObject
@property(nonatomic, weak) UIView *focused;
@property(nonatomic) CGFloat savedBorderWidth;
@property(nonatomic, strong) UIColor *savedBorderColor;
@property(nonatomic) BOOL active;
+ (instancetype)shared;
- (void)start;
- (void)stop;
@end

@implementation Vita3KPadNavigator

+ (instancetype)shared {
    static Vita3KPadNavigator *navigator;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        navigator = [[Vita3KPadNavigator alloc] init];
        [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
                                                        object:nil
                                                         queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *note) {
            [navigator wireController:note.object];
        }];
    });
    return navigator;
}

- (void)start {
    self.active = YES;
    for (GCController *controller in GCController.controllers)
        [self wireController:controller];
}

- (void)stop {
    self.active = NO;
    [self clearFocusRing];
    self.focused = nil;
}

- (void)wireController:(GCController *)controller {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad)
        return;
    __weak Vita3KPadNavigator *weakSelf = self;
    pad.dpad.up.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf move:0 dy:-1];
    };
    pad.dpad.down.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf move:0 dy:1];
    };
    pad.dpad.left.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf horizontal:-1];
    };
    pad.dpad.right.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf horizontal:1];
    };
    pad.buttonA.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf activate];
    };
    pad.buttonB.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf back];
    };
    pad.buttonY.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf gameActions];
    };
    pad.leftShoulder.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf switchTab:-1];
    };
    pad.rightShoulder.pressedChangedHandler = ^(__unused GCControllerButtonInput *b, __unused float v, BOOL pressed) {
        if (pressed) [weakSelf switchTab:1];
    };
}

- (BOOL)navigationAllowed {
    return self.active && g_library.window != nil
        && g_library.window.rootViewController.presentedViewController == nil;
}

// Topmost interactive surface: pad menu > settings > library.
- (UIView *)navigationRoot {
    for (UIView *subview in g_library.subviews.reverseObjectEnumerator) {
        if (subview.hidden)
            continue;
        if ([subview isKindOfClass:Vita3KPadMenuView.class] || [subview isKindOfClass:Vita3KSettingsView.class])
            return subview;
    }
    return g_library;
}

static void collect_candidates(UIView *view, NSMutableArray<UIView *> *out) {
    if (view.hidden || view.alpha < 0.02)
        return;
    BOOL focusable = NO;
    if ([view isKindOfClass:UIControl.class])
        focusable = ((UIControl *)view).enabled && view.userInteractionEnabled;
    else if ([view isKindOfClass:UICollectionViewCell.class])
        focusable = YES;
    if (focusable) {
        if (view.bounds.size.width >= 20 && view.bounds.size.height >= 20)
            [out addObject:view];
        return; // don't descend into a control's internals
    }
    for (UIView *subview in view.subviews)
        collect_candidates(subview, out);
}

- (NSArray<UIView *> *)candidatesIn:(UIView *)root {
    NSMutableArray<UIView *> *out = [NSMutableArray array];
    collect_candidates(root, out);
    return out;
}

static CGPoint center_in(UIView *view, UIView *root) {
    return [view convertPoint:CGPointMake(CGRectGetMidX(view.bounds), CGRectGetMidY(view.bounds)) toView:root];
}

- (void)clearFocusRing {
    UIView *old = self.focused;
    if (old) {
        old.layer.borderWidth = self.savedBorderWidth;
        old.layer.borderColor = self.savedBorderColor.CGColor;
    }
}

- (void)setFocus:(UIView *)view {
    if (!view || view == self.focused)
        return;
    [self clearFocusRing];
    self.savedBorderWidth = view.layer.borderWidth;
    self.savedBorderColor = view.layer.borderColor ? [UIColor colorWithCGColor:view.layer.borderColor] : UIColor.clearColor;
    view.layer.borderWidth = 3;
    view.layer.borderColor = UIColor.systemCyanColor.CGColor;
    if (view.layer.cornerRadius == 0)
        view.layer.cornerRadius = 10;
    self.focused = view;

    UIView *scroller = view.superview;
    while (scroller && ![scroller isKindOfClass:UIScrollView.class])
        scroller = scroller.superview;
    if (scroller) {
        const CGRect rect = [view convertRect:view.bounds toView:scroller];
        [(UIScrollView *)scroller scrollRectToVisible:CGRectInset(rect, -24, -24) animated:YES];
    }
}

- (UIView *)validFocusIn:(UIView *)root {
    UIView *view = self.focused;
    if (view && view.window && !view.hidden && [view isDescendantOfView:root])
        return view;
    return nil;
}

- (void)focusDefaultIn:(UIView *)root {
    NSArray<UIView *> *candidates = [self candidatesIn:root];
    UIView *best = nil;
    CGFloat bestScore = CGFLOAT_MAX;
    for (UIView *candidate in candidates) {
        const CGPoint center = center_in(candidate, root);
        // Prefer visible candidates, ordered top-left first.
        const BOOL visible = CGRectIntersectsRect([candidate convertRect:candidate.bounds toView:root], root.bounds);
        const CGFloat score = (visible ? 0 : 1000000) + center.y * 1000 + center.x;
        if (score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    [self setFocus:best];
}

- (void)move:(NSInteger)dx dy:(NSInteger)dy {
    if (![self navigationAllowed])
        return;
    UIView *root = [self navigationRoot];
    UIView *current = [self validFocusIn:root];
    if (!current) {
        [self focusDefaultIn:root];
        return;
    }
    const CGPoint from = center_in(current, root);
    NSArray<UIView *> *candidates = [self candidatesIn:root];
    UIView *best = nil;
    CGFloat bestScore = CGFLOAT_MAX;
    for (UIView *candidate in candidates) {
        if (candidate == current)
            continue;
        const CGPoint center = center_in(candidate, root);
        const CGFloat forward = (center.x - from.x) * dx + (center.y - from.y) * dy;
        if (forward < 8)
            continue;
        const CGFloat sideways = dx != 0 ? fabs(center.y - from.y) : fabs(center.x - from.x);
        const CGFloat score = forward + sideways * 2.5;
        if (score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    if (best)
        [self setFocus:best];
}

- (void)horizontal:(NSInteger)delta {
    if (![self navigationAllowed])
        return;
    UIView *root = [self navigationRoot];
    UIView *current = [self validFocusIn:root];
    if ([current isKindOfClass:UISlider.class]) {
        UISlider *slider = (UISlider *)current;
        const float step = (slider.maximumValue - slider.minimumValue) / 20.0f;
        slider.value = std::clamp(slider.value + step * (float)delta, slider.minimumValue, slider.maximumValue);
        [slider sendActionsForControlEvents:UIControlEventValueChanged];
        return;
    }
    if ([current isKindOfClass:UISegmentedControl.class]) {
        UISegmentedControl *segmented = (UISegmentedControl *)current;
        const NSInteger next = segmented.selectedSegmentIndex + delta;
        if (next >= 0 && next < segmented.numberOfSegments) {
            segmented.selectedSegmentIndex = next;
            [segmented sendActionsForControlEvents:UIControlEventValueChanged];
        }
        return;
    }
    [self move:delta dy:0];
}

- (void)activate {
    if (![self navigationAllowed])
        return;
    UIView *root = [self navigationRoot];
    UIView *current = [self validFocusIn:root];
    if (!current) {
        [self focusDefaultIn:root];
        return;
    }
    if ([current isKindOfClass:UICollectionViewCell.class]) {
        UIView *scroller = current.superview;
        while (scroller && ![scroller isKindOfClass:UICollectionView.class])
            scroller = scroller.superview;
        UICollectionView *collection = (UICollectionView *)scroller;
        NSIndexPath *indexPath = [collection indexPathForCell:(UICollectionViewCell *)current];
        if (indexPath && [collection.delegate respondsToSelector:@selector(collectionView:didSelectItemAtIndexPath:)])
            [collection.delegate collectionView:collection didSelectItemAtIndexPath:indexPath];
        return;
    }
    if ([current isKindOfClass:UISwitch.class]) {
        UISwitch *toggle = (UISwitch *)current;
        [toggle setOn:!toggle.on animated:YES];
        [toggle sendActionsForControlEvents:UIControlEventValueChanged];
        return;
    }
    if ([current isKindOfClass:UIControl.class])
        [(UIControl *)current sendActionsForControlEvents:UIControlEventTouchUpInside];
}

- (void)back {
    if (!self.active || g_library.window == nil)
        return;
    UIViewController *presented = g_library.window.rootViewController.presentedViewController;
    if (presented) {
        [presented dismissViewControllerAnimated:YES completion:nil];
        return;
    }
    UIView *root = [self navigationRoot];
    if ([root isKindOfClass:Vita3KPadMenuView.class]) {
        [(Vita3KPadMenuView *)root dismissMenu];
        self.focused = nil;
        return;
    }
    if ([root isKindOfClass:Vita3KSettingsView.class]) {
        [(Vita3KSettingsView *)root navigateBack];
        self.focused = nil;
    }
}

- (void)gameActions {
    if (![self navigationAllowed])
        return;
    UIView *root = [self navigationRoot];
    if (root != g_library)
        return;
    UIView *current = [self validFocusIn:root];
    if (![current isKindOfClass:UICollectionViewCell.class])
        return;
    NSIndexPath *indexPath = [g_library.collectionView indexPathForCell:(UICollectionViewCell *)current];
    if (indexPath)
        [g_library presentPadActionsForItem:indexPath.item];
}

- (void)switchTab:(NSInteger)delta {
    if (![self navigationAllowed])
        return;
    UIView *root = [self navigationRoot];
    if ([root isKindOfClass:Vita3KSettingsView.class]) {
        [(Vita3KSettingsView *)root padSwitchCategory:delta];
        self.focused = nil;
    }
}

@end

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

// Hide or reveal every CAMetalLayer-backed view under the window. The SDL
// drawable keeps the last presented game frame after a session ends, and it
// can bleed into library/settings backgrounds through composition paths that
// don't show up in screenshots. While the library UI is on screen nothing
// renders into it, so hiding it outright is safe.
static void set_metal_drawables_hidden(UIWindow *window, BOOL hidden) {
    if (!window)
        return;
    // Walk the entire window: SDL's drawable normally lives under the root
    // view controller, but a drawable parented to the window itself must be
    // caught too or it keeps showing the last game frame behind settings.
    NSMutableArray<UIView *> *pending = [NSMutableArray arrayWithObject:window];
    while (pending.count) {
        UIView *view = pending.lastObject;
        [pending removeLastObject];
        if ([view.layer isKindOfClass:CAMetalLayer.class]) {
            // Never hide a view the library lives inside: hiding an ancestor
            // hides the library with it and the whole screen goes black.
            if (hidden && g_library && [g_library isDescendantOfView:view]) {
                [pending addObjectsFromArray:view.subviews];
                continue;
            }
            view.hidden = hidden;
            continue;
        }
        [pending addObjectsFromArray:view.subviews];
    }
}

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
        g_library.frame = host.bounds;
        g_library.hidden = NO;
        [g_library updateGames:gamesCopy settings:settingsCopy];
        [g_library setJitAvailable:g_jit_available];
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        if (settingsCopy.firmware_ready) {
            // Existing installs that already contain all three packages never
            // see onboarding, even when upgrading from a build predating it.
            [defaults setBool:YES forKey:@"tsubomi.onboarded"];
            [g_library.onboardingView removeFromSuperview];
            g_library.onboardingView = nil;
        } else if (![defaults boolForKey:@"tsubomi.onboarded"] || !settingsCopy.firmware_ready) {
            if (!g_library.onboardingView) {
                g_library.onboardingView = [[Vita3KOnboardingView alloc]
                    initWithFrame:g_library.bounds settings:settingsCopy];
                [g_library addSubview:g_library.onboardingView];
            } else {
                [g_library.onboardingView updateFirmwareSettings:settingsCopy];
            }
            [g_library bringSubviewToFront:g_library.onboardingView];
        }
        [g_library.superview bringSubviewToFront:g_library];
        set_metal_drawables_hidden(window, YES);
        [Vita3KPadNavigator.shared start];
        // Session teardown may still mutate the view hierarchy after this
        // block (SDL drawable churn); reassert visibility one tick later so a
        // late-added game drawable can't cover or hide the library.
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!g_library)
                return;
            g_library.hidden = NO;
            [g_library.superview bringSubviewToFront:g_library];
            set_metal_drawables_hidden(g_library.window ?: active_window(), YES);
        });
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
        [Vita3KPadNavigator.shared stop];
        set_metal_drawables_hidden(g_library.window ?: active_window(), NO);
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

// Tap a trophy to view its art full screen (tap anywhere to dismiss).
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSDictionary *row = self.rows[indexPath.row];
    NSString *path = row[@"icon"];
    UIImage *image = path.length ? [UIImage imageWithContentsOfFile:path] : nil;
    if (!image)
        return;
    UIViewController *viewer = [[UIViewController alloc] init];
    viewer.view.backgroundColor = UIColor.blackColor;
    // OverFullScreen keeps this controller's view in place, so its
    // viewDidDisappear (which returns to the in-game menu) does not fire.
    viewer.modalPresentationStyle = UIModalPresentationOverFullScreen;
    viewer.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    UIImageView *imageView = [[UIImageView alloc] initWithImage:image];
    imageView.contentMode = UIViewContentModeScaleAspectFit;
    imageView.frame = viewer.view.bounds;
    imageView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [viewer.view addSubview:imageView];
    UILabel *caption = [[UILabel alloc] init];
    caption.text = row[@"name"];
    caption.textColor = UIColor.whiteColor;
    caption.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    caption.textAlignment = NSTextAlignmentCenter;
    caption.frame = CGRectMake(20, CGRectGetHeight(viewer.view.bounds) - 90,
        CGRectGetWidth(viewer.view.bounds) - 40, 24);
    caption.autoresizingMask = UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleWidth;
    [viewer.view addSubview:caption];
    UITapGestureRecognizer *dismiss = [[UITapGestureRecognizer alloc] initWithTarget:self
                                                                              action:@selector(dismissTrophyArt:)];
    [viewer.view addGestureRecognizer:dismiss];
    [self presentViewController:viewer animated:YES completion:nil];
}

- (void)dismissTrophyArt:(UITapGestureRecognizer *)recognizer {
    (void)recognizer;
    // Dismisses the art viewer this controller presented (not the trophy
    // sheet itself; that requires the Done button).
    if (self.presentedViewController)
        [self.presentedViewController dismissViewControllerAnimated:YES completion:nil];
}
@end

int vita3k_ios_load_fps_limit() {
    return 60;
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

@interface Vita3KFrametimeGraph : UIView
@property(nonatomic, strong) NSMutableArray<NSNumber *> *samples;
- (void)addSample:(CGFloat)value;
@end

@implementation Vita3KFrametimeGraph
- (instancetype)init {
    self = [super init];
    if (self) {
        self.backgroundColor = UIColor.clearColor;
        self.opaque = NO;
        self.samples = [NSMutableArray array];
    }
    return self;
}
- (void)addSample:(CGFloat)value {
    if (!isfinite(value) || value <= 0)
        return;
    [self.samples addObject:@(value)];
    while (self.samples.count > 60)
        [self.samples removeObjectAtIndex:0];
    [self setNeedsDisplay];
}
- (void)drawRect:(CGRect)rect {
    if (self.samples.count < 2)
        return;
    CGFloat maximum = 16.67;
    for (NSNumber *sample in self.samples)
        maximum = MAX(maximum, MIN((CGFloat)sample.doubleValue, 100.0));
    UIBezierPath *path = [UIBezierPath bezierPath];
    const CGFloat step = CGRectGetWidth(rect) / MAX((CGFloat)self.samples.count - 1, 1);
    [self.samples enumerateObjectsUsingBlock:^(NSNumber *sample, NSUInteger index, __unused BOOL *stop) {
        const CGFloat value = MIN((CGFloat)sample.doubleValue, maximum);
        CGPoint point = CGPointMake(index * step,
            CGRectGetHeight(rect) - (value / maximum) * (CGRectGetHeight(rect) - 2) - 1);
        if (index == 0)
            [path moveToPoint:point];
        else
            [path addLineToPoint:point];
    }];
    UIColor *line = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark
        ? UIColor.whiteColor : UIColor.blackColor;
    [line setStroke];
    path.lineWidth = 1.35;
    path.lineJoinStyle = kCGLineJoinRound;
    path.lineCapStyle = kCGLineCapRound;
    [path stroke];
}
- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    [super traitCollectionDidChange:previousTraitCollection];
    [self setNeedsDisplay];
}
@end

static UIVisualEffectView *g_perf_hud = nil;
static UILabel *g_perf_label = nil;
static Vita3KFrametimeGraph *g_perf_graph = nil;

void vita3k_ios_update_perf_overlay(const float guest_fps, const float frametime_ms) {
    perform_on_main(^{
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        const BOOL show_fps = [defaults boolForKey:@"vita3k.perf.fps"];
        const BOOL show_frametime = [defaults boolForKey:@"vita3k.perf.frametime"];
        const BOOL show_graph = [defaults boolForKey:@"vita3k.perf.frametimeGraph"];
        const BOOL show_ram = [defaults boolForKey:@"vita3k.perf.ram"];
        const BOOL show_battery = [defaults boolForKey:@"vita3k.perf.battery"];
        if ([defaults boolForKey:@"vita3k.perf.hidden"]
            || (!show_fps && !show_frametime && !show_graph && !show_ram && !show_battery)) {
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
            g_perf_graph = [[Vita3KFrametimeGraph alloc] init];
            [g_perf_hud.contentView addSubview:g_perf_graph];
        }
        if (g_perf_hud.superview != window) {
            [window addSubview:g_perf_hud];
            [window bringSubviewToFront:g_perf_hud];
        }
        g_perf_hud.hidden = NO;

        NSMutableArray<NSString *> *parts = [NSMutableArray array];
        if (show_fps)
            [parts addObject:[NSString stringWithFormat:@"%.0f FPS", guest_fps]];
        if (show_frametime)
            [parts addObject:frametime_ms > 0
                ? [NSString stringWithFormat:@"%.1f ms", frametime_ms] : @"-- ms"];
        if (show_ram) {
            task_vm_info_data_t vm_info{};
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO,
                    reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS)
                [parts addObject:[NSString stringWithFormat:@"%.0f MB", vm_info.phys_footprint / (1024.0 * 1024.0)]];
        }
        if (show_battery) {
            // batteryMonitoringEnabled is set once at library init, not here.
            // With monitoring enabled, modern iOS reports batteryLevel in 1%
            // steps, so show the plain percentage.
            const float level = UIDevice.currentDevice.batteryLevel;
            if (level >= 0)
                [parts addObject:[NSString stringWithFormat:@"%.0f%%", level * 100.0f]];
        }
        g_perf_label.text = [parts componentsJoinedByString:@"  ·  "];
        [g_perf_label sizeToFit];
        const CGFloat width = MAX(CGRectGetWidth(g_perf_label.bounds) + 20, show_graph ? 170.0 : 0.0);
        const CGFloat height = show_graph ? 58.0 : 24.0;
        const UIEdgeInsets safe = window.safeAreaInsets;
        const CGFloat windowWidth = CGRectGetWidth(window.bounds);
        const CGFloat windowHeight = CGRectGetHeight(window.bounds);
        const BOOL portrait = windowHeight > windowWidth;
        // User-placed position from the layout editor (normalized center, per
        // orientation); defaults: below the letterboxed game image in
        // portrait, top-left in landscape.
        NSString *keyX = portrait ? @"tsubomi.perfPos.portrait.x" : @"tsubomi.perfPos.landscape.x";
        NSString *keyY = portrait ? @"tsubomi.perfPos.portrait.y" : @"tsubomi.perfPos.landscape.y";
        CGFloat centerX;
        CGFloat centerY;
        if ([defaults objectForKey:keyX] && [defaults objectForKey:keyY]) {
            centerX = [defaults doubleForKey:keyX] * windowWidth;
            centerY = [defaults doubleForKey:keyY] * windowHeight;
        } else if (portrait) {
            const CGFloat gameHeight = windowWidth * 544.0 / 960.0;
            centerX = windowWidth / 2;
            centerY = safe.top + gameHeight + height / 2 + 10;
        } else {
            centerX = safe.left + 10 + width / 2;
            centerY = safe.top + 6 + height / 2;
        }
        centerX = std::clamp(centerX, safe.left + width / 2, windowWidth - safe.right - width / 2);
        centerY = std::clamp(centerY, safe.top + height / 2, windowHeight - safe.bottom - height / 2);
        g_perf_hud.bounds = CGRectMake(0, 0, width, height);
        g_perf_hud.center = CGPointMake(centerX, centerY);
        g_perf_label.frame = CGRectMake(10, 3, width - 20, 18);
        g_perf_graph.hidden = !show_graph;
        g_perf_graph.frame = CGRectMake(10, 27, width - 20, 25);
        if (show_graph)
            [g_perf_graph addSample:frametime_ms];
    });
}

void vita3k_ios_hide_perf_overlay() {
    perform_on_main(^{
        [g_perf_hud removeFromSuperview];
        g_perf_hud = nil;
        g_perf_label = nil;
        g_perf_graph = nil;
    });
}

void vita3k_ios_report_import_result(const std::string &message, const bool success) {
    NSString *text = [NSString stringWithUTF8String:message.c_str()] ?: @"Import finished";
    perform_on_main(^{
        [g_library hideBusyOverlay];
        [g_library.onboardingView handleImportResult:text success:success];
        if (success) {
            g_library.statusLabel.text = text;
            [g_library setNeedsLayout];
            [g_library layoutIfNeeded];
            g_library.statusGlass.alpha = 1;
            [UIView animateWithDuration:0.3 delay:6 options:0 animations:^{ g_library.statusGlass.alpha = 0; } completion:nil];
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
