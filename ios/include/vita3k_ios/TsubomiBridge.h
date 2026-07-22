// Objective-C facade between the SwiftUI frontend and the C++ core.
//
// This header is imported by the Swift bridging header, so it must stay pure
// Objective-C: no C++ types, no core headers. TsubomiBridge.mm is Objective-C++
// and does the translation to Vita3KIOSSettings / the frontend action queue on
// the other side.
//
// Keeping the boundary here (rather than turning on Swift's C++ interop) means
// the Swift side never parses the core's C++23 headers, and the set of things
// the UI can reach stays small and explicit.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/// A mutable snapshot of the emulator settings the UI can edit.
///
/// Deliberately a class, not a struct-like value: the SwiftUI layer binds to it
/// through @Observable and hands it straight back to -applySettings:.
NS_SWIFT_NAME(EmulatorSettings)
@interface TsubomiSettings : NSObject <NSCopying>

@property(nonatomic) float resolutionMultiplier;
@property(nonatomic) BOOL vSync;
@property(nonatomic) BOOL cpuOptimizations;
@property(nonatomic) BOOL ngsAudio;
@property(nonatomic) BOOL asyncPipelineCompilation;
@property(nonatomic) NSInteger anisotropicFiltering;
@property(nonatomic) BOOL highAccuracy;
@property(nonatomic) BOOL surfaceSync;
/// Real Vulkan memory mapping. Off by default: it completes some effects that
/// the iOS staging-buffer path misses, but garbles character models in titles
/// that write vertex data from shaders (Persona 4 Golden).
@property(nonatomic) BOOL doubleBuffer;

/// Physical face-button positions (0=Bottom, 1=Right, 2=Left, 3=Top). Global
/// only - a controller's button layout is a device property, not a per-game
/// preference, so the per-game editor hides these.
@property(nonatomic) NSInteger bindCross;
@property(nonatomic) NSInteger bindCircle;
@property(nonatomic) NSInteger bindSquare;
@property(nonatomic) NSInteger bindTriangle;

/// Read-only firmware state, shown but never edited by the settings UI.
@property(nonatomic, copy) NSString *firmwareVersion;
@property(nonatomic, readonly) BOOL firmwareReady;
@property(nonatomic, readonly) BOOL fontPackageReady;
@property(nonatomic, readonly) BOOL preinstalledPackageReady;
@property(nonatomic, readonly) BOOL mainFirmwareReady;
@property(nonatomic, copy) NSString *missingFirmware;

@end

/// One trophy row, already localized and formatted for display. The grade,
/// hidden-trophy masking, and unlock-date wording are resolved on the
/// Objective-C++ side so the SwiftUI layer only lays out strings.
/// Swift's Identifiable conformance is added on the Swift side (see
/// BridgeIdentifiable.swift); it is not an Objective-C protocol.
NS_SWIFT_NAME(Trophy)
@interface TsubomiTrophy : NSObject
@property(nonatomic, readonly) NSInteger trophyID;
@property(nonatomic, readonly, copy) NSString *name;
/// "Gold · Unlocked 3 Jan 2026 at 21:14" plus the description on a second line.
@property(nonatomic, readonly, copy) NSString *detail;
/// Empty when the title ships no art for this trophy.
@property(nonatomic, readonly, copy) NSString *iconPath;
@property(nonatomic, readonly) BOOL earned;
@end

NS_SWIFT_NAME(TrophyCollection)
@interface TsubomiTrophyCollection : NSObject
@property(nonatomic, readonly, copy) NSString *title;
/// "12 of 51 unlocked", or an explanation when no trophy data is installed.
@property(nonatomic, readonly, copy) NSString *progressText;
@property(nonatomic, readonly, copy) NSArray<TsubomiTrophy *> *trophies;
@end

/// What a settings screen is editing. Per-game omits the global-only rows and
/// writes a per-title override instead of committing to config.yml.
typedef NS_ENUM(NSInteger, TsubomiSettingsScope) {
    TsubomiSettingsScopeGlobal = 0,
    TsubomiSettingsScopePerGame = 1,
} NS_SWIFT_NAME(SettingsScope);

NS_SWIFT_NAME(Bridge)
@interface TsubomiBridge : NSObject

/// Current global settings, as the core last reported them.
@property(class, nonatomic, readonly) TsubomiSettings *currentSettings;

/// Settings for one title, falling back to the global value for any field the
/// title has never overridden.
// Swift names are pinned explicitly throughout: the importer's "omit needless
// words" pass would otherwise rewrite e.g. -applySettings: to apply(_:) based
// on the argument type, and the exact spelling would drift with the header.
+ (TsubomiSettings *)settingsForTitle:(NSString *)titleIdentifier
    NS_SWIFT_NAME(settings(forTitle:));

/// Queue a global settings change. Returns immediately; the core applies it on
/// its own thread and reports any restart-required fields back through the
/// existing status toast.
+ (void)applySettings:(TsubomiSettings *)settings
    NS_SWIFT_NAME(apply(_:));

/// Persist a per-title override without touching the global configuration.
+ (void)applySettings:(TsubomiSettings *)settings forTitle:(NSString *)titleIdentifier
    NS_SWIFT_NAME(apply(_:forTitle:));

/// Clear a title's overrides so it follows the global settings again.
+ (void)resetSettingsForTitle:(NSString *)titleIdentifier
    NS_SWIFT_NAME(resetSettings(forTitle:));

/// Open the bug-report form.
+ (void)openBugReportForm;

/// Present the system document picker for an official firmware .PUP. The
/// import runs asynchronously; completion is reported by the core updating the
/// firmware-ready flags, which the onboarding flow observes.
+ (void)presentFirmwareImportPicker;

/// Records that onboarding has been completed, so it is never shown again.
+ (void)markOnboardingComplete;

/// Opens the virtual on-screen controller editor (opacity, scale, layout,
/// visibility, physical-pad auto-hide). Still a UIKit screen.
+ (void)presentControllerOptions;

/// Re-reads the library display toggles (title IDs, version, size) and redraws
/// the visible cells. Called when one of those settings changes.
+ (void)reloadLibraryCells;

/// Clears `vita3k.perf.hidden` so enabling any HUD metric makes the overlay
/// visible again, matching the in-game HUD panel's behaviour.
+ (void)performanceOverlayDidEnableMetric;

/// Opens a URL in the browser (About links).
+ (void)openURLString:(NSString *)urlString NS_SWIFT_NAME(open(urlString:));

/// Cover and trophy art, decoded off the main thread and cached. `completion`
/// always runs on the main thread, and runs synchronously when the image was
/// already resident so a scrolling row never flashes a placeholder for art it
/// already has. Backed by the same cache the remaining UIKit screens use.
+ (void)loadArtAtPath:(NSString *)path
           completion:(void (^)(UIImage *_Nullable image))completion
    NS_SWIFT_NAME(loadArt(atPath:completion:));

/// Drop cached art for one path, or all of it when `path` is nil. Used after
/// the user replaces a game's cover.
+ (void)invalidateArtAtPath:(nullable NSString *)path
    NS_SWIFT_NAME(invalidateArt(atPath:));

/// Called when a trophy sheet is dismissed, so a sheet opened from the in-game
/// menu returns there instead of dropping the user back into the game.
+ (void)trophySheetDidDismiss;

/// Display string for the installed firmware, or nil when none is installed.
@property(class, nonatomic, readonly, nullable) NSString *firmwareVersionDisplay;

@end

NS_ASSUME_NONNULL_END
