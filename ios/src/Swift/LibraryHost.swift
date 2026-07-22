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
        // Opaque: this view sits over the game's Metal drawable, and anything
        // it does not paint is a window onto the last frame the game rendered.
        // LibraryView paints its own background too; both are deliberate.
        controller.view.backgroundColor = .systemBackground
        controller.view.isOpaque = true
        return controller
    }
}
