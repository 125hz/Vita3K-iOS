import SwiftUI
import UIKit

/// Objective-C entry point to the SwiftUI settings screen.
///
/// The frontend is still UIKit/SDL-driven, so screens are migrated one at a
/// time: this hands back a plain `UIViewController` that the existing
/// Objective-C++ presenter shows exactly like the view controller it replaces.
@objc(TsubomiSettingsHost)
@MainActor
final class SettingsHost: NSObject {

    /// Global settings.
    ///
    /// - Parameter onDismiss: called after the user taps Done and the change
    ///   has been queued, so the caller can restore whatever chrome it hid.
    @objc(globalSettingsViewControllerWithDismissHandler:)
    static func globalSettingsViewController(
        onDismiss: @escaping () -> Void
    ) -> UIViewController {
        make(scope: .global, onDismiss: onDismiss)
    }

    /// Per-game overrides for one title.
    @objc(settingsViewControllerForTitle:displayName:dismissHandler:)
    static func settingsViewController(
        forTitle titleID: String,
        displayName: String,
        onDismiss: @escaping () -> Void
    ) -> UIViewController {
        make(scope: .perGame(titleID: titleID, displayName: displayName), onDismiss: onDismiss)
    }

    private static func make(
        scope: SettingsModel.Scope,
        onDismiss: @escaping () -> Void
    ) -> UIViewController {
        // `controller` is captured by the view's finish handler, so it has to
        // exist first; the box breaks the initialization cycle.
        let box = ControllerBox()
        let view = SettingsView(scope: scope) {
            box.controller?.dismiss(animated: true) {
                LibraryState.shared.showSavedSettingsToast()
                onDismiss()
            }
        }
        let controller = UIHostingController(rootView: view)
        box.controller = controller
        // The emulator keeps rendering behind a partially-detented sheet, so
        // present full-height: settings is a modal task, not an inspector.
        controller.modalPresentationStyle = .formSheet
        if let sheet = controller.sheetPresentationController {
            sheet.detents = [.large()]
            sheet.prefersGrabberVisible = false
        }
        return controller
    }

    private final class ControllerBox {
        weak var controller: UIViewController?
    }
}
