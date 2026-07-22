import SwiftUI
import UIKit

/// Objective-C entry point to the SwiftUI on-screen controller.
///
/// The returned controller's view is transparent and must be added over the
/// game's Metal view. Touches in the gaps between controls fall through to it
/// (see ControlTouchSurface), which is how Vita touchscreen input still works.
@objc(TsubomiControlsHost)
@MainActor
final class ControlsHost: NSObject {

    @objc(controlsViewControllerWithMenuHandler:)
    static func controlsViewController(onMenuTap: @escaping () -> Void) -> UIViewController {
        let controller = UIHostingController(rootView: ControlsOverlayView(onMenuTap: onMenuTap))
        // The overlay is chrome over a live drawable: it must never paint a
        // background of its own, or the game disappears behind it.
        controller.view.backgroundColor = .clear
        controller.view.isOpaque = false
        // Let the game's own drawable own the safe area; controls are placed
        // by normalized position across the full screen.
        controller.safeAreaRegions = []
        return controller
    }

    /// Enters or leaves the drag-to-reposition editor.
    @objc(setLayoutEditing:)
    static func setLayoutEditing(_ editing: Bool) {
        ControlsModel.shared.isEditing = editing
    }

    @objc static var isLayoutEditing: Bool {
        ControlsModel.shared.isEditing
    }

    /// Mirrors the physical-controller state so `hideWhenPhysical` can act.
    @objc(setPhysicalControllerConnected:)
    static func setPhysicalControllerConnected(_ connected: Bool) {
        ControlsModel.shared.physicalControllerConnected = connected
    }

    /// Drops every held input, for a session pausing with controls down.
    @objc static func releaseAllInputs() {
        ControlsModel.shared.pressedControls.removeAll()
        ControlsModel.shared.stickOffsets.removeAll()
        ControllerInput.releaseAll()
    }

    /// Restores the built-in control layout.
    @objc static func resetLayout() {
        ControlsModel.shared.resetLayout()
    }
}
