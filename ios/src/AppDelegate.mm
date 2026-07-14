#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/IOSLogger.h>

@interface VitaViewController : UIViewController
@end

@implementation VitaViewController

- (void)loadView {
    MTKView *view = [[MTKView alloc] initWithFrame:CGRectZero device:MTLCreateSystemDefaultDevice()];
    view.clearColor = MTLClearColorMake(0.035, 0.055, 0.09, 1.0);
    view.enableSetNeedsDisplay = YES;
    view.paused = YES;
    self.view = view;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    const auto status = vita3k::ios::query_core_status();
    vita3k::ios::log_message("INFO", status.summary);

    UILabel *title = [[UILabel alloc] init];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.text = @"Vita3K iOS bootstrap";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.textAlignment = NSTextAlignmentCenter;

    UILabel *details = [[UILabel alloc] init];
    details.translatesAutoresizingMaskIntoConstraints = NO;
    details.text = [NSString stringWithUTF8String:status.summary.c_str()];
    details.textColor = [UIColor colorWithWhite:0.78 alpha:1.0];
    details.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    details.numberOfLines = 0;
    details.textAlignment = NSTextAlignmentCenter;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, details]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16.0;
    [self.view addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24.0],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24.0],
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:680.0]
    ]];
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;
    (void)launchOptions;
    vita3k::ios::initialize_logging();

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[VitaViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)applicationWillResignActive:(UIApplication *)application {
    (void)application;
    vita3k::ios::log_message("INFO", "Application will resign active");
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    (void)application;
    vita3k::ios::log_message("INFO", "Application entered background");
}

- (void)applicationWillTerminate:(UIApplication *)application {
    (void)application;
    vita3k::ios::log_message("INFO", "Application will terminate");
}

@end
