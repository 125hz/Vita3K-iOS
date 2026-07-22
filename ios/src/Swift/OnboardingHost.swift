import SwiftUI
import UIKit

/// Objective-C entry point to the SwiftUI onboarding flow.
///
/// Returns a controller whose view the library adds as a subview (rather than
/// presenting it), matching how the UIKit flow was hosted: onboarding must sit
/// inside the library's own view so rotation and the launch intro animation
/// continue to treat it as part of that hierarchy.
@objc(TsubomiOnboardingHost)
@MainActor
final class OnboardingHost: NSObject {

    @objc(onboardingViewControllerWithFinishHandler:)
    static func onboardingViewController(onFinish: @escaping () -> Void) -> UIViewController {
        let controller = UIHostingController(rootView: OnboardingView(onFinish: onFinish))
        // The flow draws its own full-bleed background; without this the
        // hosting controller's default background would punch a white/black
        // rectangle over the library during the launch intro's scale-up.
        controller.view.backgroundColor = .clear
        return controller
    }
}
