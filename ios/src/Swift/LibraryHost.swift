import SwiftUI
import UIKit

/// Objective-C entry point to the SwiftUI library.
///
/// The library is the app's root content, so this hands back a controller the
/// frontend installs as the window's root view controller — replacing the
/// Vita3KLibraryView that used to be added as a subview.
@objc(TsubomiLibraryHost)
@MainActor
final class LibraryHost: NSObject {

    @objc static func libraryViewController() -> UIViewController {
        let controller = UIHostingController(rootView: LibraryView())
        // The onboarding flow is added over this controller's view and draws
        // its own full-bleed background.
        controller.view.backgroundColor = .systemBackground
        return controller
    }
}
