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
