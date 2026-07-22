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

    /// Computed rather than stored: touching a @MainActor singleton from a
    /// struct's property initializer would be an isolation violation.
    /// @Observable tracks the reads either way.
    private var firmware: FirmwareState { FirmwareState.shared }

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
        Page(
            symbol: "shippingbox",
            title: "Install PREINSTALL.PUP",
            body: """
                Choose the official PREINSTALL.PUP from your own Vita firmware files. \
                This installs the preinstalled system content required by games.
                """,
            requirement: .preinstalled
        ),
        Page(
            symbol: "textformat",
            title: "Install FONTPKG.PUP",
            body: """
                Choose the official FONTPKG.PUP. This installs the Vita system fonts used \
                by games and the emulator.
                """,
            requirement: .fontPackage
        ),
        Page(
            symbol: "gearshape.2",
            title: "Install PSVUPDAT.PUP",
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
        ZStack {
            // Flat adaptive background: black in dark mode, white in light.
            Color(.systemBackground).ignoresSafeArea()

            card
                .frame(maxWidth: isCompact ? 400 : 360)
                .padding(.horizontal, isCompact ? 40 : 24)
        }
        // Forward-only: there is no back affordance, and the flow cannot be
        // dismissed interactively.
        .interactiveDismissDisabled()
    }

    private var card: some View {
        VStack(spacing: isCompact ? 12 : 18) {
            if pageIndex > 0 {
                Image(systemName: page.symbol)
                    .font(.system(size: isCompact ? 34 : 44))
                    .foregroundStyle(.cyan)
                    .accessibilityHidden(true)
            }

            Text(page.title)
                .font(isCompact ? .title2 : .title)
                .fontWeight(.bold)
                .multilineTextAlignment(.center)

            if !page.body.isEmpty {
                // Scrolls rather than truncating, which is what kept breaking
                // on short landscape phones at large Dynamic Type sizes.
                ScrollView {
                    Text(page.body)
                        .font(isCompact ? .footnote : .subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: .infinity)
                }
                .scrollBounceBehavior(.basedOnSize)
            }

            if page.requirement != nil && requirementSatisfied {
                Label("Installed", systemImage: "checkmark.circle.fill")
                    .font(.footnote)
                    .foregroundStyle(.green)
                    .transition(.opacity)
            }

            actions
        }
        .padding(isCompact ? 20 : 28)
        .frame(maxWidth: .infinity)
        .background(.regularMaterial, in: .rect(cornerRadius: 28, style: .continuous))
        // Page changes cross-fade; the UIKit version slid and sprang the stack,
        // but an animation the user cannot skip on a mandatory flow is friction.
        .animation(.snappy(duration: 0.25), value: pageIndex)
        .animation(.snappy(duration: 0.25), value: requirementSatisfied)
    }

    @ViewBuilder
    private var actions: some View {
        VStack(spacing: 10) {
            if page.requirement != nil {
                Button("Choose PUP") {
                    Bridge.presentFirmwareImportPicker()
                }
                .buttonStyle(.borderedProminent)
            }

            if isLastPage {
                Button("Get Started") {
                    Bridge.markOnboardingComplete()
                    onFinish()
                }
                .buttonStyle(.borderedProminent)
                // The last page still gates on all three packages: a user who
                // somehow reached it without them must not get into the library.
                .disabled(!firmware.allPackagesReady)
            } else {
                Button("Next") {
                    pageIndex += 1
                }
                .buttonStyle(.bordered)
                .disabled(!requirementSatisfied)
            }
        }
        .controlSize(isCompact ? .regular : .large)
        .frame(maxWidth: .infinity)
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

        func isSatisfied(by state: FirmwareState) -> Bool {
            switch self {
            case .preinstalled: return state.preinstalledReady
            case .fontPackage: return state.fontPackageReady
            case .mainFirmware: return state.mainFirmwareReady
            }
        }
    }
}
