// Objective-C++ half of the SwiftUI bridge. Everything C++ stops here; see
// TsubomiBridge.h for why the boundary is drawn at this file.

#import "vita3k_ios/TsubomiBridge.h"

#import <UIKit/UIKit.h>

#include <string>
#include <utility>

#include "vita3k_ios/NativeFrontend.h"
#include "vita3k_ios/NativeFrontendInternal.h"
#include "vita3k_ios/VirtualController.h"

namespace {

NSString *to_ns(const std::string &value) {
    return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

std::string to_std(NSString *value) {
    return value.UTF8String ? std::string(value.UTF8String) : std::string();
}

NSString *trophy_grade_name(int grade) {
    switch (grade) {
    case 1: return @"Platinum";
    case 2: return @"Gold";
    case 3: return @"Silver";
    case 4: return @"Bronze";
    default: return @"Trophy";
    }
}

} // namespace

@interface TsubomiTrophy ()
- (instancetype)initWithEntry:(const Vita3KIOSTrophyEntry &)entry
                    formatter:(NSDateFormatter *)formatter;
@end

@implementation TsubomiTrophy

- (instancetype)initWithEntry:(const Vita3KIOSTrophyEntry &)entry
                    formatter:(NSDateFormatter *)formatter {
    self = [super init];
    if (!self)
        return nil;
    _trophyID = entry.id;
    _earned = entry.earned;
    _iconPath = to_ns(entry.icon_path);

    NSString *name = to_ns(entry.name);
    NSString *detail = to_ns(entry.detail);
    // A hidden trophy the player has not earned must not leak its name or
    // description; earning it reveals both.
    if (entry.hidden && !entry.earned) {
        name = @"Hidden trophy";
        detail = @"Unlock this trophy to reveal its details.";
    }
    _name = name.length ? name : @"Trophy";

    NSString *state = @"Locked";
    if (entry.earned && entry.timestamp > 0) {
        NSDate *date = [NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(entry.timestamp)];
        state = [NSString stringWithFormat:@"Unlocked %@", [formatter stringFromDate:date]];
    }
    NSString *summary = [NSString stringWithFormat:@"%@ · %@", trophy_grade_name(entry.grade), state];
    _detail = detail.length ? [NSString stringWithFormat:@"%@\n%@", summary, detail] : summary;
    return self;
}

@end

@interface TsubomiTrophyCollection ()
- (instancetype)initWithCollection:(const Vita3KIOSTrophyCollection &)collection;
@end

@implementation TsubomiTrophyCollection

- (instancetype)initWithCollection:(const Vita3KIOSTrophyCollection &)collection {
    self = [super init];
    if (!self)
        return nil;
    NSString *title = to_ns(collection.title);
    _title = title.length ? title : @"Trophies";
    _progressText = collection.total > 0
        ? [NSString stringWithFormat:@"%d of %d unlocked", collection.unlocked, collection.total]
        : @"No trophy data is installed for this title yet.";

    // One formatter for the whole collection: NSDateFormatter construction is
    // expensive and a list can run to a hundred rows.
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.dateStyle = NSDateFormatterMediumStyle;
    formatter.timeStyle = NSDateFormatterShortStyle;

    NSMutableArray<TsubomiTrophy *> *rows =
        [NSMutableArray arrayWithCapacity:collection.trophies.size()];
    for (const auto &entry : collection.trophies)
        [rows addObject:[[TsubomiTrophy alloc] initWithEntry:entry formatter:formatter]];
    _trophies = rows;
    return self;
}

@end

// The conversions traffic in C++ types, so they live in a class extension here
// rather than in the (strictly Objective-C) public header. Swift never sees
// them; it only ever receives an already-converted TsubomiSettings.
@interface TsubomiSettings ()
- (instancetype)initWithCoreSettings:(const Vita3KIOSSettings &)core;
- (Vita3KIOSSettings)coreSettings;
@end

@implementation TsubomiSettings

// Designated conversion in: keeps the Objective-C property names (which the
// Swift layer sees) decoupled from the core's field names.
- (instancetype)initWithCoreSettings:(const Vita3KIOSSettings &)core {
    self = [super init];
    if (!self)
        return nil;
    _resolutionMultiplier = core.resolution_multiplier;
    _vSync = core.v_sync;
    _cpuOptimizations = core.cpu_opt;
    _ngsAudio = core.ngs_enable;
    _asyncPipelineCompilation = core.async_pipeline_compilation;
    _anisotropicFiltering = core.anisotropic_filtering;
    _highAccuracy = core.high_accuracy;
    _surfaceSync = core.surface_sync;
    _doubleBuffer = core.double_buffer;
    _bindCross = core.bind_cross;
    _bindCircle = core.bind_circle;
    _bindSquare = core.bind_square;
    _bindTriangle = core.bind_triangle;
    _firmwareVersion = to_ns(core.firmware_version);
    _firmwareReady = core.firmware_ready;
    _fontPackageReady = core.font_package_ready;
    _preinstalledPackageReady = core.preinstalled_package_ready;
    _mainFirmwareReady = core.main_firmware_ready;
    _missingFirmware = to_ns(core.missing_firmware);
    return self;
}

// Conversion out. The read-only firmware fields are carried straight back
// through: the settings screen never edits them, but the action the core
// receives is a whole Vita3KIOSSettings, so dropping them would clear the
// library header's firmware display on every save.
- (Vita3KIOSSettings)coreSettings {
    Vita3KIOSSettings core;
    core.resolution_multiplier = self.resolutionMultiplier;
    core.v_sync = self.vSync;
    core.fps_limit = 60; // iOS always requests 60; the limiter UI was removed.
    core.cpu_opt = self.cpuOptimizations;
    core.ngs_enable = self.ngsAudio;
    core.async_pipeline_compilation = self.asyncPipelineCompilation;
    core.anisotropic_filtering = static_cast<int>(self.anisotropicFiltering);
    core.high_accuracy = self.highAccuracy;
    core.surface_sync = self.surfaceSync;
    core.double_buffer = self.doubleBuffer;
    core.bind_cross = static_cast<int>(self.bindCross);
    core.bind_circle = static_cast<int>(self.bindCircle);
    core.bind_square = static_cast<int>(self.bindSquare);
    core.bind_triangle = static_cast<int>(self.bindTriangle);
    core.firmware_version = to_std(self.firmwareVersion);
    core.firmware_ready = self.firmwareReady;
    core.font_package_ready = self.fontPackageReady;
    core.preinstalled_package_ready = self.preinstalledPackageReady;
    core.main_firmware_ready = self.mainFirmwareReady;
    core.missing_firmware = to_std(self.missingFirmware);
    return core;
}

- (id)copyWithZone:(NSZone *)zone {
    (void)zone;
    const Vita3KIOSSettings core = [self coreSettings];
    return [[TsubomiSettings alloc] initWithCoreSettings:core];
}

@end

namespace vita3k_ios_internal {

id bridge_trophies(const Vita3KIOSTrophyCollection &collection) {
    return [[TsubomiTrophyCollection alloc] initWithCollection:collection];
}

} // namespace vita3k_ios_internal

@implementation TsubomiBridge

+ (TsubomiSettings *)currentSettings {
    return [[TsubomiSettings alloc]
        initWithCoreSettings:vita3k_ios_internal::current_global_settings()];
}

+ (TsubomiSettings *)settingsForTitle:(NSString *)titleIdentifier {
    const auto global = vita3k_ios_internal::current_global_settings();
    return [[TsubomiSettings alloc]
        initWithCoreSettings:vita3k_ios_internal::load_title_settings(titleIdentifier, global)];
}

+ (void)applySettings:(TsubomiSettings *)settings {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ApplySettings;
    action.settings = [settings coreSettings];
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)applySettings:(TsubomiSettings *)settings forTitle:(NSString *)titleIdentifier {
    // Per-game overrides are persisted locally and picked up at the next
    // launch (see the Launch action's has_settings_override). Deliberately no
    // ApplySettings action: that path commits to the global config.
    vita3k_ios_internal::store_title_settings(titleIdentifier, [settings coreSettings]);
}

+ (void)resetSettingsForTitle:(NSString *)titleIdentifier {
    vita3k_ios_internal::clear_title_settings(titleIdentifier);
}

+ (void)openBugReportForm {
    NSURL *url = [NSURL URLWithString:@"https://forms.gle/PRE5MoNocokpyNNJA"];
    if (url)
        [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
}

+ (void)presentFirmwareImportPicker {
    vita3k_ios_internal::present_firmware_picker();
}

+ (void)markOnboardingComplete {
    [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"tsubomi.onboarded"];
}

+ (void)presentControllerOptions {
    vita3k_ios_present_controller_options();
}

+ (void)reloadLibraryCells {
    vita3k_ios_internal::reload_library();
}

+ (void)performanceOverlayDidEnableMetric {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"vita3k.perf.hidden"];
}

+ (void)openURLString:(NSString *)urlString {
    NSURL *url = [NSURL URLWithString:urlString];
    if (url)
        [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
}

+ (void)loadArtAtPath:(NSString *)path completion:(void (^)(UIImage *_Nullable))completion {
    // An empty path never reaches the decode queue, so report it here; every
    // other route through cached_art reports exactly once (resident image
    // returned directly, or `ready` invoked with the decode result or nil).
    // Callers bridge this to an async await, which would hang on a miss.
    if (!path.length) {
        completion(nil);
        return;
    }
    UIImage *resident = vita3k_ios_internal::cached_art(path, ^(UIImage *decoded) {
        completion(decoded);
    });
    if (resident)
        completion(resident);
}

+ (void)invalidateArtAtPath:(NSString *)path {
    vita3k_ios_internal::invalidate_cached_art(path);
}

+ (void)trophySheetDidDismiss {
    vita3k_ios_submenu_dismissed();
}

+ (NSString *)firmwareVersionDisplay {
    const auto settings = vita3k_ios_internal::current_global_settings();
    if (settings.firmware_version.empty())
        return nil;
    return to_ns(settings.firmware_version);
}

@end
