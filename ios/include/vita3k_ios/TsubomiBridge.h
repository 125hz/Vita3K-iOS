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

/// One installed title, as the library shows it.
///
/// Unlike the trophy rows, the metadata here is exposed as separate formatted
/// fields rather than one pre-composed string: the library's display toggles
/// (title IDs, version, size) decide which lines appear, and SwiftUI composes
/// them so a toggle change re-renders without going back through the core.
///
/// Swift's Identifiable conformance is added in BridgeIdentifiable.swift.
NS_SWIFT_NAME(GameEntry)
@interface TsubomiGameEntry : NSObject
/// PCSG00291-style identifier; the library's natural key.
@property(nonatomic, readonly, copy) NSString *titleID;
/// The user's rename override when set, otherwise the title from the package.
@property(nonatomic, readonly, copy) NSString *displayTitle;
/// Cover art path — the custom cover when one has been set, else the packaged
/// icon. Empty when neither exists.
@property(nonatomic, readonly, copy) NSString *iconPath;
/// "v1.01", or "Unknown version".
@property(nonatomic, readonly, copy) NSString *versionText;
/// "3h 12m", "<1m", or "0m".
@property(nonatomic, readonly, copy) NSString *playedTimeText;
/// A localized short date, or "Never played".
@property(nonatomic, readonly, copy) NSString *lastPlayedText;
/// Formatted with NSByteCountFormatter, e.g. "3.2 GB".
@property(nonatomic, readonly, copy) NSString *sizeText;
/// 0 total means the title ships no trophy data.
@property(nonatomic, readonly) NSInteger trophiesUnlocked;
@property(nonatomic, readonly) NSInteger trophiesTotal;
/// True when the user has saved per-game setting overrides for this title.
@property(nonatomic, readonly) BOOL hasSettingsOverrides;
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

#pragma mark - Library actions

/// Boot a title. The caller is responsible for having checked that firmware is
/// installed and JIT is attached; the library gates on both.
+ (void)launchTitle:(NSString *)titleID NS_SWIFT_NAME(launch(titleID:));

/// Ask the core to rescan installed titles.
+ (void)refreshLibrary;

/// Re-derive the library entries from the core's last snapshot. Used after a
/// frontend-only change (a rename) that the core has no new data for.
+ (NSArray<TsubomiGameEntry *> *)libraryEntries;

/// Frontend-only rename. An empty string clears the override so the packaged
/// title comes back.
+ (void)setDisplayTitle:(NSString *)title forTitle:(NSString *)titleID
    NS_SWIFT_NAME(setDisplayTitle(_:forTitle:));

/// Remove the installed game, its update and DLC. Saves, licenses and trophy
/// progress are kept so a reinstall picks them back up.
+ (void)deleteTitle:(NSString *)titleID NS_SWIFT_NAME(delete(titleID:));

/// Load and show this title's trophies.
+ (void)requestTrophiesForTitle:(NSString *)titleID NS_SWIFT_NAME(requestTrophies(titleID:));

/// Document pickers. Each returns immediately; results arrive as a status
/// toast or an alert.
+ (void)presentGameImportPicker;
+ (void)presentLicenseImportPicker;
+ (void)presentSaveImportPickerForTitle:(NSString *)titleID
    NS_SWIFT_NAME(presentSaveImportPicker(titleID:));
+ (void)exportSaveForTitle:(NSString *)titleID NS_SWIFT_NAME(exportSave(titleID:));

/// Shows the graphics-help explainer from the library header.
+ (void)presentGraphicsHelp;

/// Explains that a JIT-enabling debugger must be attached before a game can
/// boot. Shown instead of launching when JIT is unavailable.
+ (void)presentJITRequiredAlert;

/// Opens the global settings sheet from the library header.
+ (void)presentGlobalSettings;

/// Opens the per-game settings sheet.
+ (void)presentSettingsForTitle:(NSString *)titleID displayName:(NSString *)displayName
    NS_SWIFT_NAME(presentSettings(forTitle:displayName:));

/// Shown when the user tries to import a game before firmware is installed.
/// Returns NO and presents an explanatory alert when firmware is missing.
+ (BOOL)firmwareReadyOrPresentAlert;

/// Leaves the drag-to-reposition editor. Routed through here rather than
/// flipping the model's flag directly: an editing session started from the
/// library has a preview overlay to tear down, which only the presenting side
/// knows about.
+ (void)finishLayoutEditing;

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

#pragma mark - Virtual gamepad

/// The SDL virtual joystick behind the on-screen controls.
///
/// Called from the touch surface on every touch event, so these are kept as
/// thin as possible: no allocation, no dispatch, straight through to SDL.
NS_SWIFT_NAME(VirtualPad)
@interface TsubomiVirtualPad : NSObject

/// `button` is an SDL_GamepadButton raw value.
+ (void)setButton:(int32_t)button pressed:(BOOL)pressed
    NS_SWIFT_NAME(setButton(_:pressed:));

/// `axis` is an SDL_GamepadAxis raw value; `value` is the SDL axis range.
+ (void)setAxis:(int32_t)axis value:(int16_t)value
    NS_SWIFT_NAME(setAxis(_:value:));

/// Clears every button and axis. Used when a session pauses with controls held
/// down, so nothing stays stuck on.
+ (void)releaseAllInputs;

/// The overlay's measured top safe-area inset, in pixels. The core reads this
/// to letterbox the guest image below the notch, and cannot ask SwiftUI for it.
+ (void)reportSafeAreaTopPixels:(float)pixels
    NS_SWIFT_NAME(reportSafeAreaTopPixels(_:));

@end

NS_ASSUME_NONNULL_END
