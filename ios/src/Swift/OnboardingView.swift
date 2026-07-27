import SwiftUI

/// Mandatory first-run flow: six forward-only pages, three of which gate on an
/// official firmware package actually being installed.
///
/// The UIKit version carried ~250 lines of constraint work to keep the card
/// legible across rotation — an explicit safe-area-derived width, separate
/// landscape metrics, `preferredMaxLayoutWidth` refreshes, and manual
/// `invalidateIntrinsicContentSize` calls. None of that is needed here: the
/// layout is expressed once and adapts through the size class, and text wraps
/// and scrolls on its own. That is the main reason this screen was worth
/// migrating.
/// Main-actor isolated as a whole: it reads the @MainActor FirmwareState, and
/// a SwiftUI view's body is main-actor anyway, so this just makes the helpers
/// that body calls agree with it.
@MainActor
struct OnboardingView: View {
    /// Called once the user finishes the last page with firmware installed.
    let onFinish: () -> Void

    @State private var pageIndex = 0

    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    /// Computed rather than stored: touching a @MainActor singleton from a
    /// struct's property initializer would be an isolation violation.
    /// @Observable tracks the reads either way.
    private var firmware: FirmwareState { FirmwareState.shared }

    /// The install runs on the emulator thread and reports progress through the
    /// library's busy state. Onboarding covers the library, so that indicator
    /// is not visible from here - without surfacing it, choosing a firmware
    /// file looks like it did nothing at all.
    private var installProgress: String? { LibraryState.shared.busyMessage }

    /// Short phones in landscape get tighter metrics so every page still fits.
    private var isCompact: Bool { verticalSizeClass == .compact }

    private static let pages: [Page] = [
        Page(
            symbol: "sparkles",
            title: "Welcome to Tsubomi",
            body: "",
            requirement: nil
        ),
        Page(
            symbol: "checkmark.shield",
            title: "Bring Your Own Games",
            body: """
                Piracy is not supported. You must supply your own legally obtained game dumps \
                and license files; Tsubomi does not include games, firmware, keys, or licenses.
                """,
            requirement: nil
        ),
        // Titles name what the user is installing rather than the filename;
        // the hint still says which file to pick.
        Page(
            symbol: "shippingbox",
            title: "Install Pre-Install Firmware",
            body: "Choose the official pre-install firmware PUP.",
            requirement: .preinstalled
        ),
        Page(
            symbol: "textformat",
            title: "Install Font Firmware",
            body: "Choose the official font package PUP.",
            requirement: .fontPackage
        ),
        Page(
            symbol: "gearshape.2",
            title: "Install Firmware",
            body: "Choose the official PSVUPDAT.PUP. This installs the main Vita system firmware.",
            requirement: .mainFirmware
        ),
        Page(
            symbol: "flask",
            title: "Experimental Software",
            body: """
                Not every game works yet. Expect graphics glitches, crashes, missing features, \
                and performance issues. Please keep useful logs when something breaks.
                """,
            requirement: nil
        ),
    ]

    private var page: Page { Self.pages[pageIndex] }
    private var isLastPage: Bool { pageIndex == Self.pages.count - 1 }

    /// A firmware page cannot be advanced past until its package is installed.
    private var requirementSatisfied: Bool {
        guard let requirement = page.requirement else { return true }
        return requirement.isSatisfied(by: firmware)
    }

    var body: some View {
        // No card. Apple's own first-run flows put the content on the plain
        // background with the actions pinned to the bottom edge; a material
        // panel over an opaque background is an invisible rectangle that only
        // costs vertical space, which is what left the screen mostly empty.
        VStack(spacing: 0) {
            Spacer(minLength: 0)
            pageContent
            Spacer(minLength: 0)
            actions
        }
        .padding(.horizontal, isCompact ? 60 : 32)
        .padding(.bottom, isCompact ? 16 : 32)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(.systemBackground).ignoresSafeArea())
        // Forward-only: there is no back affordance, and the flow cannot be
        // dismissed interactively.
        .interactiveDismissDisabled()
    }

    private var pageContent: some View {
        VStack(spacing: isCompact ? 10 : 16) {
            if pageIndex > 0 {
                Image(systemName: page.symbol)
                    .font(.system(size: isCompact ? 44 : 60))
                    .foregroundStyle(.tint)
                    // Symbols are how the user perceives the page changing;
                    // a bounce on arrival reads as the step advancing.
                    .symbolEffect(.bounce, value: pageIndex)
                    .accessibilityHidden(true)
            }

            Text(page.title)
                .font(isCompact ? .title2 : .largeTitle)
                .fontWeight(.bold)
                .multilineTextAlignment(.center)

            if !page.body.isEmpty {
                Text(page.body)
                    .font(isCompact ? .footnote : .body)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    // Natural height: no ScrollView, which is greedy and was
                    // what stretched this down the screen.
                    .fixedSize(horizontal: false, vertical: true)
            }

            if page.requirement != nil && requirementSatisfied {
                Label("Installed", systemImage: "checkmark.circle.fill")
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(.green)
                    .symbolEffect(.bounce, value: requirementSatisfied)
                    .transition(.scale.combined(with: .opacity))
            }
        }
        // Pages slide in from the trailing edge, matching a forward-only flow.
        .id(pageIndex)
        .transition(.asymmetric(
            insertion: .move(edge: .trailing).combined(with: .opacity),
            removal: .move(edge: .leading).combined(with: .opacity)
        ))
        .animation(reduceMotion ? nil : .snappy(duration: 0.3), value: pageIndex)
        .animation(reduceMotion ? nil : .snappy(duration: 0.25), value: requirementSatisfied)
    }

    @ViewBuilder
    private var actions: some View {
        // One prominent action per page. Where a page has both, "Choose" is
        // the primary and "Next" steps back to plain glass, so exactly one
        // element on screen carries the tint.
        VStack(spacing: 12) {
            if let installProgress {
                // Replaces the button outright rather than sitting beside it:
                // a second file cannot be chosen while one is installing.
                HStack(spacing: 8) {
                    ProgressView()
                    Text(installProgress)
                        .font(.subheadline)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
                .glassEffect(.regular, in: .capsule)
                .transition(.opacity)
            } else if page.requirement != nil {
                // Before the package is installed, choosing a file is the job.
                // Once it lands, the job is to continue - so the tint moves to
                // Next below and this steps back to plain glass. Without that
                // the finished page still points at the button the user has
                // already used, and Next reads as unavailable.
                if requirementSatisfied {
                    Button("Choose Firmware File") {
                        Bridge.presentFirmwareImportPicker()
                    }
                    .buttonStyle(.glass)
                } else {
                    Button("Choose Firmware File") {
                        Bridge.presentFirmwareImportPicker()
                    }
                    .buttonStyle(.glassProminent)
                }
            }

            if isLastPage {
                Button("Get Started") {
                    Bridge.markOnboardingComplete()
                    onFinish()
                }
                .buttonStyle(.glassProminent)
                // The last page still gates on all three packages: a user who
                // somehow reached it without them must not get into the library.
                .disabled(!firmware.allPackagesReady)
            } else if page.requirement == nil {
                // No firmware button on this page, so Next is the primary.
                Button("Next") { pageIndex += 1 }
                    .buttonStyle(.glassProminent)
            } else if requirementSatisfied {
                // The package is in: this is now the only thing left to do.
                Button("Next") { pageIndex += 1 }
                    .buttonStyle(.glassProminent)
            } else {
                // Plain glass and disabled: "Choose Firmware File" above is
                // the primary until its package is installed, and only one
                // element per screen should carry the tint.
                Button("Next") { pageIndex += 1 }
                    .buttonStyle(.glass)
                    .disabled(true)
            }

            progressDots
        }
        .controlSize(.large)
        .frame(maxWidth: .infinity)
        .animation(reduceMotion ? nil : .snappy(duration: 0.25), value: requirementSatisfied)
        .animation(reduceMotion ? nil : .snappy(duration: 0.25), value: installProgress)
    }

    /// Page indicator. Forward-only, so the dots are a progress readout rather
    /// than a control - they are not tappable and are hidden from VoiceOver in
    /// favour of the announcement below.
    private var progressDots: some View {
        HStack(spacing: 6) {
            ForEach(0..<Self.pages.count, id: \.self) { index in
                Capsule()
                    .fill(index == pageIndex ? AnyShapeStyle(.tint) : AnyShapeStyle(.quaternary))
                    .frame(width: index == pageIndex ? 20 : 6, height: 6)
            }
        }
        .padding(.top, 4)
        .animation(reduceMotion ? nil : .snappy(duration: 0.3), value: pageIndex)
        .accessibilityElement()
        .accessibilityLabel("Step \(pageIndex + 1) of \(Self.pages.count)")
    }

    // MARK: - Page model

    private struct Page {
        let symbol: String
        let title: String
        let body: String
        /// nil for informational pages, which can always be advanced.
        let requirement: FirmwareRequirement?
    }

    private enum FirmwareRequirement {
        case preinstalled
        case fontPackage
        case mainFirmware

        // A nested type does not inherit the enclosing type's actor
        // isolation, so this needs @MainActor of its own to read FirmwareState.
        @MainActor
        func isSatisfied(by state: FirmwareState) -> Bool {
            switch self {
            case .preinstalled: return state.preinstalledReady
            case .fontPackage: return state.fontPackageReady
            case .mainFirmware: return state.mainFirmwareReady
            }
        }
    }
}
