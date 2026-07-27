// Objective-C++ half of the SwiftUI bridge. Everything C++ stops here; see
// TsubomiBridge.h for why the boundary is drawn at this file.

#include <vita3k_ios/NativeFrontend.h>
#include <vita3k_ios/VirtualController.h>
#include <util/log.h>

// Same MacTypes collision the frontend hits: Apple's MacTypes.h declares
// `typedef char *Ptr;`, which clashes with the emulator's global Ptr<T>
// template forward-declared by util/log.h above. util/log.h must come first,
// then MacTypes is pulled in (fully, and include-guarded) inside this renamed
// region, so the project headers below - which import UIKit themselves - are
// no-ops by the time they are reached.
#define Ptr MacTypesPtr
#import <UIKit/UIKit.h>
#undef Ptr

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "vita3k_ios/NativeFrontendInternal.h"
#import "vita3k_ios/TsubomiBridge.h"

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

@interface TsubomiGameEntry ()
- (instancetype)initWithEntry:(const Vita3KIOSGameEntry &)entry;
@end

@implementation TsubomiGameEntry

- (instancetype)initWithEntry:(const Vita3KIOSGameEntry &)entry {
    self = [super init];
    if (!self)
        return nil;
    NSString *identifier = to_ns(entry.title_id);
    _titleID = identifier.length ? identifier : @"Unknown title ID";

    // A title from a package with invalid UTF-8 would otherwise arrive as nil
    // and render as an empty row; name it after its ID instead and log the
    // bytes so the offending package can be identified.
    NSString *title = [NSString stringWithUTF8String:entry.title.c_str()];
    if (!title) {
        LOG_ERROR("iOS library title is invalid UTF-8: title_id={} bytes={}",
            entry.title_id, vita3k_ios_internal::hex_bytes_for_log(entry.title));
        title = [NSString stringWithFormat:@"Unknown title (%@)", _titleID];
    }
    _displayTitle = vita3k_ios_internal::display_title_for(_titleID, title);

    _iconPath = to_ns(entry.icon_path);
    _wideArtPath = to_ns(entry.wide_art_path);
    _liveAreaContentsPath = to_ns(entry.live_area_contents_path);
    _hasSettingsOverrides = vita3k_ios_internal::title_has_settings(_titleID);
    _playedTimeSeconds = std::max<int64_t>(0, entry.time_played_seconds);
    _lastPlayedTimestamp = std::max<int64_t>(
        0, static_cast<int64_t>(entry.last_played_timestamp));

    NSString *version = to_ns(entry.version);
    _versionText = version.length ? [@"v" stringByAppendingString:version] : @"Unknown version";

    const long long minutes = MAX(0, entry.time_played_seconds) / 60;
    if (entry.time_played_seconds > 0 && minutes == 0)
        _playedTimeText = @"<1m";
    else if (minutes >= 60)
        _playedTimeText = [NSString stringWithFormat:@"%lldh %lldm", minutes / 60, minutes % 60];
    else
        _playedTimeText = [NSString stringWithFormat:@"%lldm", minutes];

    if (entry.last_played_timestamp <= 0) {
        _lastPlayedText = @"Never played";
    } else {
        NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
        formatter.dateStyle = NSDateFormatterShortStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
        _lastPlayedText = [formatter stringFromDate:
            [NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(entry.last_played_timestamp)]];
    }

    NSByteCountFormatter *bytes = [[NSByteCountFormatter alloc] init];
    bytes.countStyle = NSByteCountFormatterCountStyleFile;
    _sizeText = [bytes stringFromByteCount:static_cast<long long>(entry.size_bytes)];

    _trophiesUnlocked = entry.trophies_unlocked;
    _trophiesTotal = entry.trophies_total;
    return self;
}

@end

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
    _trophySetID = to_ns(collection.trophy_id);
    _canEdit = collection.can_edit;
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
    _shaderCache = core.shader_cache;
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
    core.shader_cache = self.shaderCache;
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

id bridge_settings() {
    return [[TsubomiSettings alloc] initWithCoreSettings:current_global_settings()];
}

id bridge_games() {
    const auto games = current_games();
    NSMutableArray<TsubomiGameEntry *> *rows = [NSMutableArray arrayWithCapacity:games.size()];
    for (const auto &game : games)
        [rows addObject:[[TsubomiGameEntry alloc] initWithEntry:game]];
    return rows;
}

} // namespace vita3k_ios_internal

@implementation TsubomiVirtualPad

+ (void)setButton:(int32_t)button pressed:(BOOL)pressed {
    vita3k_ios_virtual_pad_set_button(button, pressed);
}

+ (void)setAxis:(int32_t)axis value:(int16_t)value {
    vita3k_ios_virtual_pad_set_axis(axis, value);
}

+ (void)releaseAllInputs {
    vita3k_ios_virtual_pad_release_all();
}

+ (void)setVitaTouchscreenEnabled:(BOOL)enabled {
    vita3k_ios_set_vita_touchscreen_enabled(enabled);
}

+ (void)reportSafeAreaTopPixels:(float)pixels {
    vita3k_ios_report_safe_area_top_pixels(pixels);
}

@end

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

+ (void)launchTitle:(NSString *)titleID {
    const auto game = vita3k_ios_internal::game_for_title(titleID);
    if (!game) {
        // The row was stale - the library refreshed the title out from under
        // the tap. Doing nothing is correct; the list has already updated.
        LOG_WARN("iOS launch requested for a title that is no longer installed: {}",
            titleID.UTF8String ? titleID.UTF8String : "");
        return;
    }
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::Launch;
    action.app_path = game->app_path;
    // Per-game overrides are applied for this session only; see the Launch
    // handling of has_settings_override in UpstreamMain.cpp.
    if (vita3k_ios_internal::title_has_settings(titleID)) {
        action.settings = vita3k_ios_internal::load_title_settings(
            titleID, vita3k_ios_internal::current_global_settings());
        action.has_settings_override = true;
    }
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)refreshLibrary {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::Refresh;
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (NSArray<TsubomiGameEntry *> *)libraryEntries {
    return vita3k_ios_internal::bridge_games();
}

+ (void)setDisplayTitle:(NSString *)title forTitle:(NSString *)titleID {
    vita3k_ios_internal::set_display_title(titleID, title);
}

+ (void)deleteTitle:(NSString *)titleID {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::DeleteGame;
    action.title_id = to_std(titleID);
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)requestTrophiesForTitle:(NSString *)titleID {
    const auto game = vita3k_ios_internal::game_for_title(titleID);
    if (!game)
        return;
    // The ShowTrophies action carries the display title, the title id, and the
    // separate trophy id (the NPWR… the trophy data is filed under).
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ShowTrophies;
    action.title_id = game->title;
    action.app_path = game->title_id;
    action.trophy_id = game->trophy_id;
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)setTrophy:(NSInteger)trophyID
           earned:(BOOL)earned
     collectionID:(NSString *)collectionID {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::SetTrophyState;
    action.trophy_id = to_std(collectionID);
    action.trophy_entry_id = static_cast<int>(trophyID);
    action.trophy_earned = earned;
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)presentGameImportPicker {
    vita3k_ios_internal::present_game_picker();
}

+ (void)presentLicenseImportPicker {
    vita3k_ios_internal::present_license_import_picker();
}

+ (void)presentSaveImportPickerForTitle:(NSString *)titleID {
    vita3k_ios_internal::present_save_import_picker(titleID);
}

+ (void)exportSaveForTitle:(NSString *)titleID {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ExportSave;
    action.title_id = to_std(titleID);
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)shareLogFile {
    vita3k_ios_share_log_file();
}

+ (void)presentAllSaveImportPicker {
    vita3k_ios_internal::present_all_save_import_picker();
}

+ (void)exportAllSaves {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ExportSave;
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)presentLibraryArchiveImportPicker {
    vita3k_ios_internal::present_library_archive_import_picker();
}

+ (void)exportLibraryArchive {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ExportLibraryArchive;
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)exportGameArchiveForTitle:(NSString *)titleID {
    Vita3KIOSFrontendAction action;
    action.kind = Vita3KIOSFrontendActionKind::ExportGameArchive;
    action.title_id = to_std(titleID);
    vita3k_ios_internal::queue_frontend_action(std::move(action));
}

+ (void)presentGraphicsHelp {
    vita3k_ios_internal::show_graphics_help();
}

+ (void)presentJITRequiredAlert {
    vita3k_ios_internal::show_jit_required_alert();
}

+ (void)presentGlobalSettings {
    vita3k_ios_internal::present_settings_sheet(@"", @"");
}

+ (void)presentSettingsForTitle:(NSString *)titleID displayName:(NSString *)displayName {
    vita3k_ios_internal::present_settings_sheet(titleID, displayName);
}

+ (BOOL)firmwareReadyOrPresentAlert {
    return vita3k_ios_internal::firmware_ready_or_alert();
}

+ (void)finishLayoutEditing {
    vita3k_ios_finish_layout_editing();
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

+ (void)applyOrientationLock:(NSString *)orientation {
    [NSUserDefaults.standardUserDefaults setObject:orientation
                                           forKey:@"tsubomi.orientationLock"];
    vita3k_ios_apply_orientation_lock();
}

+ (void)setOrientationLockEnabled:(BOOL)enabled {
    [NSUserDefaults.standardUserDefaults setBool:enabled
                                          forKey:@"tsubomi.orientationLockEnabled"];
    vita3k_ios_apply_orientation_lock();
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
