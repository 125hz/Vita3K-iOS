import SwiftUI
import UIKit

/// Objective-C entry point to the SwiftUI on-screen controller.
///
/// The returned controller's view is transparent and must be added over the
/// game's Metal view. Touches in the gaps between controls fall through to it
/// (see ControlTouchSurface), which is how Vita touchscreen input still works.
/// Hosting controller that reports its top safe-area inset to the core.
///
/// The core letterboxes the guest image below the notch using this value, and
/// UIKit's view.safeAreaInsets is the authoritative source that updates on
/// rotation - more reliable than reading it back through a SwiftUI
/// GeometryReader, which reports zero once the content goes full-bleed.
private final class ControlsHostingController<Content: View>: UIHostingController<Content> {
    override func viewSafeAreaInsetsDidChange() {
        super.viewSafeAreaInsetsDidChange()
        let scale = view.window?.screen.nativeScale ?? UIScreen.main.nativeScale
        SafeAreaReporter.topPixels = Float(view.safeAreaInsets.top * scale)
    }
}

@objc(TsubomiControlsHost)
@MainActor
final class ControlsHost: NSObject {

    @objc(controlsViewControllerWithMenuHandler:)
    static func controlsViewController(onMenuTap: @escaping () -> Void) -> UIViewController {
        let controller = ControlsHostingController(rootView: ControlsOverlayView(onMenuTap: onMenuTap))
        // The overlay is chrome over a live drawable: it must never paint a
        // background of its own, or the game disappears behind it.
        controller.view.backgroundColor = .clear
        controller.view.isOpaque = false
        return controller
    }

    /// Enters or leaves the drag-to-reposition editor.
    @objc(setLayoutEditing:)
    static func setLayoutEditing(_ editing: Bool) {
        ControlsModel.shared.isEditing = editing
    }

    /// Called by the editor's Done button. Routed through the C side because a
    /// session started from the library has a preview overlay to tear down,
    /// which only that side knows about.
    static func finishLayoutEditing() {
        Bridge.finishLayoutEditing()
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

    /// Hides the floating menu button, from the in-game menu's own action.
    @objc(setMenuButtonVisible:)
    static func setMenuButtonVisible(_ visible: Bool) {
        let model = ControlsModel.shared
        // Written to both layouts: the button is chrome, and hiding it in
        // landscape only to have it reappear on rotation would read as a bug.
        for orientation in ["landscape", "portrait"] {
            model.setMenuVisible(visible, in: orientation)
        }
    }

    /// Whether the menu button is currently hidden, so the three-finger tap
    /// knows if it has anything to restore.
    @objc static var isMenuButtonHidden: Bool {
        !ControlsModel.shared.isMenuVisibleInAnyLayout
    }

    /// Reports the window's top safe-area inset in pixels. The core reads this
    /// to letterbox the guest image below the notch.
    @objc(setSafeAreaTopPixels:)
    static func setSafeAreaTopPixels(_ pixels: Float) {
        SafeAreaReporter.topPixels = pixels
    }
}

/// Bridges the overlay's measured safe area to the C++ side, which cannot ask
/// SwiftUI for it.
@MainActor
enum SafeAreaReporter {
    static var topPixels: Float = 0 {
        didSet {
            guard topPixels != oldValue else { return }
            VirtualPad.reportSafeAreaTopPixels(topPixels)
        }
    }
}
