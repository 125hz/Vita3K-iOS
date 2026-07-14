#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/IOSLogger.h>

@interface VitaViewController : UIViewController
@property(nonatomic, strong) UILabel *detailsLabel;
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

    UILabel *title = [[UILabel alloc] init];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.text = @"Vita3K iOS — Core Milestone 6";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.textAlignment = NSTextAlignmentCenter;

    self.detailsLabel = [[UILabel alloc] init];
    self.detailsLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.detailsLabel.textColor = [UIColor colorWithWhite:0.78 alpha:1.0];
    self.detailsLabel.font = [UIFont monospacedSystemFontOfSize:14.0 weight:UIFontWeightRegular];
    self.detailsLabel.numberOfLines = 0;
    self.detailsLabel.textAlignment = NSTextAlignmentLeft;

    UIButton *rescanButton = [UIButton buttonWithType:UIButtonTypeSystem];
    rescanButton.translatesAutoresizingMaskIntoConstraints = NO;
    [rescanButton setTitle:@"Rescan Imports" forState:UIControlStateNormal];
    [rescanButton addTarget:self action:@selector(rescanImports) forControlEvents:UIControlEventTouchUpInside];
    rescanButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.detailsLabel, rescanButton]];
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

    [self refreshStatus];
}

- (void)refreshStatus {
    const auto status = vita3k::ios::query_core_status();
    self.detailsLabel.text = [NSString stringWithUTF8String:status.summary.c_str()];
}

- (void)rescanImports {
    const auto status = vita3k::ios::rescan_imports();
    vita3k::ios::log_message("INFO", status.summary);
    [self refreshStatus];
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
    const auto core_status = vita3k::ios::initialize_core(vita3k::ios::log_file_path().parent_path());
    vita3k::ios::log_message("INFO", core_status.summary);

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
