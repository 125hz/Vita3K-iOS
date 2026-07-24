import SwiftUI
import UIKit

/// Objective-C entry point to the SwiftUI trophy list.
@objc(TsubomiTrophyHost)
@MainActor
final class TrophyHost: NSObject {

    @objc(trophyViewControllerForCollection:)
    static func trophyViewController(for collection: TrophyCollection) -> UIViewController {
        TrophyState.shared.update(collection)
        let box = ControllerBox()
        let view = TrophyListView {
            box.controller?.dismiss(animated: true) {
                // Opened from the in-game menu, dismissing returns there. The
                // UIKit screen did this in viewDidDisappear, which also fired
                // when it merely presented the art viewer on top of itself;
                // driving it from the explicit Done action instead means the
                // art viewer no longer trips it.
                Bridge.trophySheetDidDismiss()
            }
        }
        let controller = UIHostingController(rootView: view)
        box.controller = controller
        controller.modalPresentationStyle = .pageSheet
        return controller
    }

    private final class ControllerBox {
        weak var controller: UIViewController?
    }
}
