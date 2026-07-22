import SwiftUI
import UIKit

/// Objective-C entry points for the in-game sheets.
///
/// Each returns a controller the caller presents; none of them retain state of
/// their own, so a sheet dismissed and reopened always reflects current config.
@objc(TsubomiGameOverlayHosts)
@MainActor
final class GameOverlayHosts: NSObject {

    /// The in-game menu.
    ///
    /// The handlers are separate blocks rather than one "action" enum because
    /// each has a different dismissal rule: Resume and Quit close the menu,
    /// Controller Options and Trophies replace it and return to it afterwards,
    /// and Hide Menu Button acts without any further screen.
    @objc(gameMenuViewControllerWithResume:editLayout:trophies:performanceHUD:hideMenuButton:quit:)
    static func gameMenuViewController(
        onResume: @escaping () -> Void,
        onEditLayout: @escaping () -> Void,
        onTrophies: @escaping () -> Void,
        onPerformanceHUD: @escaping () -> Void,
        onHideMenuButton: @escaping () -> Void,
        onQuit: @escaping () -> Void
    ) -> UIViewController {
        host(GameMenuView(
            onResume: onResume,
            onEditLayout: onEditLayout,
            onTrophies: onTrophies,
            onPerformanceHUD: onPerformanceHUD,
            onHideMenuButton: onHideMenuButton,
            onQuit: onQuit
        ))
    }

    @objc(controllerOptionsViewControllerWithEditLayout:finish:)
    static func controllerOptionsViewController(
        onEditLayout: @escaping () -> Void,
        onFinish: @escaping () -> Void
    ) -> UIViewController {
        host(ControllerOptionsView(onEditLayout: onEditLayout, onFinish: onFinish))
    }

    @objc(performanceHUDPanelViewControllerWithFinish:)
    static func performanceHUDPanelViewController(
        onFinish: @escaping () -> Void
    ) -> UIViewController {
        host(PerformanceHUDPanel(onFinish: onFinish))
    }

    /// A transparent hosting controller for the always-on performance readout.
    /// Its view is added over the game and must not intercept touches.
    @objc static func performanceOverlayViewController() -> UIViewController {
        let controller = UIHostingController(rootView: PerformanceOverlayView())
        controller.view.backgroundColor = .clear
        controller.view.isOpaque = false
        controller.view.isUserInteractionEnabled = false
        controller.safeAreaRegions = []
        return controller
    }

    private static func host(_ view: some View) -> UIViewController {
        let controller = UIHostingController(rootView: view)
        // The game keeps rendering behind these sheets; a clear background lets
        // the sheet's own material be what the user sees through.
        controller.view.backgroundColor = .clear
        controller.modalPresentationStyle = .pageSheet
        return controller
    }
}
