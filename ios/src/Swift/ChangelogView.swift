import SwiftUI

/// Release notes.
///
/// Previously a UIAlertController with one long concatenated string, which
/// could not scroll properly once the list grew past a screen. A real scrolling
/// list also lets each release be its own section.
@MainActor
struct ChangelogView: View {
    var body: some View {
        List {
            ForEach(Changelog.releases) { release in
                Section(release.version) {
                    ForEach(Array(release.notes.enumerated()), id: \.offset) { _, note in
                        Text(note)
                            .font(.subheadline)
                    }
                }
            }
        }
        .navigationTitle("What's New")
        .navigationBarTitleDisplayMode(.inline)
    }
}

enum AppInfo {
    /// "0.23.0 (230)" from the bundle, so it can never drift from Info.plist.
    static var versionDisplay: String {
        let info = Bundle.main.infoDictionary
        let short = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "\(short) (\(build))"
    }
}

/// STANDING RULE: add a new entry at the top of `releases` and bump
/// ios/Info.plist.in's version and build before every commit batch.
enum Changelog {
    struct Release: Identifiable {
        let version: String
        let notes: [String]
        var id: String { version }
    }

    static let releases: [Release] = [
        Release(version: "0.46.2", notes: [
            "A failed export now names what it was doing and what the archiver reported, instead of blaming free space for every failure.",
            "Export failures are titled Export failed rather than Import failed.",
        ]),
        Release(version: "0.46.1", notes: [
            "Exporting games or saves now closes Settings and shows progress. It ran silently behind the sheet before, so a long export looked like the button had done nothing.",
        ]),
        Release(version: "0.46.0", notes: [
            "Export games now works for a whole library. The archive was capped at 4 GB, so it failed at whichever file crossed that line; exports are zip64 now and can be any size.",
            "Exporting no longer deflates game data that cannot compress, which makes a large export dramatically faster, and it checks there is room for the archive first instead of failing partway through.",
            "Orientation Lock has moved out of the in-game menu into Layout Options, and Layout now sits directly below Appearance there.",
            "Liquid Glass has moved to Settings › General.",
            "The JIT warning is a small centered badge instead of a full-width bar.",
            "Less background work while a game runs: the log console is skipped when nothing is reading it, the logger thread runs on the efficiency cores, and playtime is checkpointed every two minutes instead of every thirty seconds.",
        ]),
        Release(version: "0.45.0", notes: [
            "Settings and Layout Options can turn off Liquid Glass in game. The controls, menu button, performance overlay and live log are drawn as flat translucent shapes instead, which removes a per-frame backdrop sample for every one of them. The library, onboarding and settings keep Liquid Glass either way.",
            "The performance tick no longer wakes the main thread once a second when every metric is switched off.",
        ]),
        Release(version: "0.44.0", notes: [
            "Layout Options can turn on Dynamic Joystick: the on-screen sticks are hidden, and each half of the screen becomes a stick that centers itself wherever you place your finger and lifts when you let go. Pressing a button never raises one.",
            "Dynamic Joystick disables the Vita touchscreen, because the controller then owns the whole screen. The setting says so before you turn it on, and turning it back off restores touch input.",
            "V-Sync now chooses the Vulkan present mode instead of being ignored. Left on, the renderer is paced by the display rather than running ahead of it, which cuts battery drain over a long session.",
            "The boot diagnostic watchdog retires once a game is running, instead of waking the CPU every second and writing to the log for the rest of the session.",
        ]),
        Release(version: "0.43.0", notes: [
            "All-game-save archives now carry save data, trophy progress, playtime, and last-played dates, and restore each type when imported.",
        ]),
        Release(version: "0.42.0", notes: [
            "Live Area now uses Vita3K's original 960×544 authored layouts, scaling the complete background, frames, gate, and Start control onto both portrait and landscape screens without scrolling.",
            "Orientation Lock now sits directly below Layout Options in the in-game menu.",
            "Library settings can export every game save to one ZIP or import matching saves from an all-saves archive.",
            "The Library category now follows General and can sort every library view by Alphabetical, Title ID, Playtime, or Recently Played.",
        ]),
        Release(version: "0.41.0", notes: [
            "Live Area artwork, title, frames, and Start control are smaller in both portrait and landscape.",
            "Rotating while Live Area is open no longer leaves the library wider than the portrait window after dismissal.",
            "Library settings can choose Cover Art or Game Icon for the normal list; Compact list continues to use game icons.",
            "The in-game menu now has the full blue Orientation Lock control with Portrait, Landscape, and Landscape (Flipped) choices.",
        ]),
        Release(version: "0.40.1", notes: [
            "Fixed the iPhoneOS SwiftUI compile failure in the animated JIT status banner.",
        ]),
        Release(version: "0.40.0", notes: [
            "Transient home notifications now float above the library without moving the games.",
            "Wide artwork is used in the regular list as well as cards and carousel; Compact list always uses each game's square icon0.png.",
            "The in-game menu now includes Portrait Lock, and trophy symbols use an adaptive grey instead of gold.",
            "Hold a trophy in the home trophy browser to lock or unlock it; the open list and library count update after the progress file is saved.",
            "Game menus can copy “Game name [Title ID]” and open a package-backed Live Area with its authored background, gate, frames, text, and Start action.",
        ]),
        Release(version: "0.39.0", notes: [
            "Orientation lock can now be enabled in General settings, with Portrait, Landscape, and flipped Landscape directions.",
            "The JIT warning is shorter and stays legible in both appearances.",
            "Opening Settings no longer nudges the recessed library upward, and wide covers slightly overscan their rounded frame so square source edges cannot show.",
            "Done now confirms Saved settings, compact list rows include the last-played date and time, and Report a bug is highlighted in red.",
        ]),
        Release(version: "0.38.2", notes: [
            "The home controls are compact again in one system-managed Liquid Glass toolbar, with list/card view on the left, Add centered, and Settings on the right.",
            "List/card switching and reopening Settings after saving now use the native toolbar interaction path.",
            "The recessed library keeps rounded corners during interactive sheet drags, and its depth animation is synchronized with UIKit to remove the opening jump.",
        ]),
        Release(version: "0.38.1", notes: [
            "The home controls now share one continuous Liquid Glass capsule, with list/card view on the left, Add centered, and Settings on the right.",
            "Opening a home modal now recesses and dims the library more deeply, synchronized with the system sheet transition; in-game screens and confirmation alerts are unchanged.",
            "Pull-to-refresh now plays its ready cue as soon as each refresh is accepted, followed by success when the rescan finishes.",
        ]),
        Release(version: "0.38.0", notes: [
            "Home-screen settings, trophies, pickers, and other modal screens now dim and recess the visible library; alerts and context-menu confirmations stay in place.",
            "Settings starts with a General section for Interface sound effects, uses a centered title, and the home bar places list/card view on the left with Add centered.",
            "Rapid pull-to-refresh gestures no longer discard their ready and success cues, and Add plus the list/card toggle now play the press cue.",
            "Wide cover art always returns to each game's packaged 16:9 artwork with consistent card sizes. Custom cover picking and cropping have been removed.",
            "The transient Tsubomi startup screen has been removed.",
        ]),
        Release(version: "0.37.0", notes: [
            "Opening a modal now subtly scales and rounds the screen behind it for a stronger sense of depth; Reduce Motion keeps the standard system transition.",
            "Library-only interaction sounds now mark carousel selection, game launch, imports, license requirements, settings, and refresh completion. They are enabled by default and can be disabled in Settings › Library.",
            "Pull down from the grid, list, empty library, or landscape carousel to rescan the library. The old bottom-bar Refresh button is removed.",
            "Adjust cover crop is unavailable while Wide cover art is enabled, because wide cards display the full image instead of the square crop.",
        ]),
        Release(version: "0.36.0", notes: [
            "Physical and on-screen analog sticks now translate circular SDL output to the Vita's independent-axis range, so full diagonal movement no longer drops to roughly 71% per axis.",
            "Partial stick travel remains analog, and desktop and Android controller behavior is unchanged.",
        ]),
        Release(version: "0.35.1", notes: [
            "Fixes the iPhoneOS compile failure in the new Vita touchscreen input diagnostics.",
        ]),
        Release(version: "0.35.0", notes: [
            "Touches between the on-screen controls now pass through the SwiftUI overlay to the Vita front touchscreen, including Gravity Rush touch interactions.",
            "Device logs now record Vita touch-down and touch-up delivery for input troubleshooting.",
        ]),
        Release(version: "0.34.1", notes: [
            "Fixes the iPhoneOS compile failure in the variable-width cover carousel.",
        ]),
        Release(version: "0.34.0", notes: [
            "Grid cards are visible again in Light Mode, using an adaptive system surface with clear separation from the library background.",
            "Wide cover art is substantially larger in the landscape carousel.",
            "Rapid D-pad input can no longer leave the centred cover and the game selected by Cross out of sync.",
            "Game launch preparation is single-shot and exception-safe, with a visible recovery message instead of partial session state.",
            "Quitting no longer waits on a guest graphics callback before asking guest threads to stop, removing a teardown deadlock.",
            "Startup, onboarding and controller-driven scrolling now honor Reduce Motion, and the cover editor follows Dynamic Type.",
        ]),
        Release(version: "0.33.0", notes: [
            "Wide cover art is on by default and now shows the full banner in the grid and the landscape carousel.",
            "New Settings › Library › Compact list, and the regular list's cover art is larger.",
            "Hold a game and you can reset a custom cover back to the packaged art.",
            "On-screen ✕ and △ are a bit larger, and the square is a lighter pink so it no longer reads like the circle.",
            "Colored face buttons can also be toggled from the in-game Layout Options.",
            "Fixed: a game controller could still drive the library while a game's context menu was open, so Cross launched a game instead of picking a menu item.",
            "Fixed carousel D-pad double-taps registering twice, and the performance graph no longer stretches the whole overlay.",
        ]),
        Release(version: "0.32.0", notes: [
            "The whole on-screen controller, the in-game menu, controller options and the performance overlay are now SwiftUI with real Liquid Glass. The menu button is always visible and can be dragged anywhere without entering the editor; the performance overlay can be repositioned in Layout Options.",
            "On-screen ✕ ○ □ △ glyphs are colored (blue, red, pink, green). Turn it off in Settings › Controls.",
            "New Settings › Library › Wide cover art shows the full Vita banner art instead of cropping it to a square.",
            "The default control layout is spread out and hand-tuned so the glass buttons no longer merge together.",
            "The game is no longer clipped by the notch in portrait, and quitting a game no longer occasionally freezes.",
            "Battery and smoothness pass: analog-stick input and library browsing do far less redraw work.",
        ]),
        Release(version: "0.27.0", notes: [
            "Fixed a launch crash on a fresh install, and a stale frame of a quit game showing behind the library.",
            "The library title is centered and its controls moved to a Liquid Glass bar at the bottom of the screen.",
            "The landscape cover carousel loops endlessly in one direction with a controller, and its covers are larger.",
            "Grid cards are all the same size, and a refresh shows a quick spin and confirmation.",
        ]),
        Release(version: "0.26.0", notes: [
            "Tsubomi now requires iOS 26 — the interface is built around Liquid Glass, so the old fallback look is gone.",
            "Settings, Trophies and first-run setup were rebuilt in SwiftUI ahead of the rest of the app.",
        ]),
        Release(version: "0.25.0", notes: [
            "The game library is now SwiftUI. Grid, list and the landscape cover carousel are rebuilt on system components, so scrolling, context menus and rotation all behave the way they do elsewhere in iOS.",
            "Fixed the carousel opening with every cover at full brightness — the games beside the selected one are now dimmed immediately instead of only after the first scroll.",
            "Game controller navigation is rebuilt: the D-pad moves a focus ring through the library, Cross launches, Triangle opens the game's actions, Circle clears the ring.",
            "The trophy count in the games list is now aligned with the text beside it.",
            "The firmware version no longer sits in the top-right of the library — it lives in Settings › About, and the space goes to your games.",
        ]),
        Release(version: "0.24.0", notes: [
            """
            Tsubomi now requires iOS 26. The interface is built around Liquid Glass, and supporting \
            earlier versions meant shipping a second, non-glass presentation of every surface for \
            devices that cannot run the design anyway.
            """,
            "On-screen controls lose the hard white outline around each button — it only ever existed to give the older blur effect an edge that Liquid Glass draws for itself.",
        ]),
        Release(version: "0.23.2", notes: [
            "Second build fix. Everything listed under 0.23.0 arrives here; 0.23.0 and 0.23.1 never produced a working build.",
        ]),
        Release(version: "0.23.1", notes: [
            "Build fix attempt for 0.23.0 (superseded).",
        ]),
        Release(version: "0.23.0", notes: [
            """
            Fixed Persona 4 Golden's garbled character models properly. The 0.20.0 "double buffer" \
            memory mapping is now off by default, and there is a new Graphics › Double buffer switch \
            (global and per-game) if a game needs it — Gravity Rush is the one that does.
            """,
            """
            The 0.21.0 attempt at this relied on memory-write faults that a device log proves are \
            never delivered while a JIT debugger is attached, so it could not have worked. Those page \
            protections are no longer installed on iOS.
            """,
            """
            Settings, Trophies and the first-run setup are rebuilt in SwiftUI: standard grouped \
            sections, system controls throughout, and Liquid Glass now comes from the system \
            navigation bars instead of being drawn behind every row. Dynamic Type, VoiceOver and \
            Reduce Transparency come along with it.
            """,
            "Trophy art no longer decodes on the main thread, so a long trophy list scrolls smoothly the first time through.",
            """
            First-run setup adapts to rotation and large text sizes on its own, instead of relying on \
            hand-tuned landscape measurements that could clip titles.
            """,
            "The landscape cover carousel now dims the covers beside the selected game the moment it opens, instead of only after the first scroll.",
            "The carousel also gives a small haptic tick as each cover passes under the center.",
            "Known regression: a game controller cannot navigate the Settings screen for now; touch is required there.",
        ]),
        Release(version: "0.22.0", notes: [
            "Big battery and smoothness pass. On-screen controls no longer run a live glass refraction pass per element every frame of gameplay — that was the app's single largest GPU cost.",
            "Library covers are decoded once off the main thread and cached, so scrolling a large library is smooth instead of stuttering on every row.",
            "The library screen no longer wakes the CPU 62 times a second while you sit browsing, and a background timer that ran forever now stops after startup settles.",
            "Liquid Glass cleanup: glass is reserved for bars, HUDs, menus and controls, rounded cards use continuous corners, leftover pre-iOS 26 blur materials are gone, and tint is reserved for the JIT warning.",
            "Control presses tint the glass instead of painting a flat chip over it, and the hard white outline around each control is gone on iOS 26.",
        ]),
        Release(version: "0.21.0", notes: [
            "Attempted fix for Persona 4 Golden's garbled character models (superseded in 0.23.0 — it did not work on device).",
            "Added Settings › Face button layout: remap which physical face button triggers Cross/Circle/Square/Triangle, for controllers that report them in the wrong position.",
            "Save export bundles play time along with trophy progress; importing a save restores it too.",
            "Trophy counts and play time refresh immediately after importing a save.",
            "Fixed a lingering frozen frame of a just-quit game sometimes showing behind Settings.",
        ]),
        Release(version: "0.20.0", notes: [
            """
            Experimental real GPU memory mapping ("double buffer") on iOS — fixed Gravity Rush's black \
            environment and textures. Now opt-in; see 0.23.0.
            """,
        ]),
        Release(version: "0.19.0", notes: [
            "Added an optional live log panel (Settings › Performance overlay › Show live log).",
            "App launch fades and scales the library in instead of popping onto screen.",
            "Fixed a controller D-pad navigation bug where scrolling the landscape carousel made covers drift vertically.",
        ]),
    ]
}
