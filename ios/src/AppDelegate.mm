#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vita3k_ios/CoreBridge.h>
#import <vita3k_ios/IOSInputAdapter.h>
#include <vita3k_ios/IOSLogger.h>
#import <vita3k_ios/IOSMetalRenderer.h>

@interface VitaViewController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) UILabel *detailsLabel;
@property(nonatomic, strong) UIButton *addGameButton;
@property(nonatomic, strong) VitaInputAdapter *inputAdapter;
@property(nonatomic, strong) VitaMetalRenderer *renderer;
@end

@implementation VitaViewController

- (void)loadView {
    MTKView *view = [[MTKView alloc] initWithFrame:CGRectZero device:MTLCreateSystemDefaultDevice()];
    view.clearColor = MTLClearColorMake(0.035, 0.055, 0.09, 1.0);
    view.enableSetNeedsDisplay = YES;
    view.multipleTouchEnabled = YES;
    view.paused = YES;
    self.view = view;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    UILabel *title = [[UILabel alloc] init];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.text = @"Vita3K iOS - Core Milestone 15";
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

    self.addGameButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.addGameButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.addGameButton setTitle:@"Add Game ZIP/VPK" forState:UIControlStateNormal];
    [self.addGameButton addTarget:self action:@selector(addGame) forControlEvents:UIControlEventTouchUpInside];
    self.addGameButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];

    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[self.addGameButton, rescanButton]];
    buttons.axis = UILayoutConstraintAxisHorizontal;
    buttons.distribution = UIStackViewDistributionFillEqually;
    buttons.spacing = 24.0;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.detailsLabel, buttons]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16.0;

    UIScrollView *scrollView = [[UIScrollView alloc] init];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.alwaysBounceVertical = YES;
    [self.view addSubview:scrollView];
    [scrollView addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [scrollView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [scrollView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:scrollView.contentLayoutGuide.leadingAnchor constant:24.0],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:scrollView.contentLayoutGuide.trailingAnchor constant:-24.0],
        [stack.topAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.topAnchor constant:24.0],
        [stack.bottomAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.bottomAnchor constant:-24.0],
        [stack.centerXAnchor constraintEqualToAnchor:scrollView.frameLayoutGuide.centerXAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:680.0]
    ]];

    __weak VitaViewController *weakSelf = self;
    self.renderer = [[VitaMetalRenderer alloc] initWithView:(MTKView *)self.view completion:^{
        [weakSelf refreshStatus];
    }];
    self.inputAdapter = [[VitaInputAdapter alloc] initWithView:self.view completion:^{
        [weakSelf refreshStatus];
    }];

    [self refreshStatus];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self.inputAdapter attachTouchSurface];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self.renderer requestFirstFrame];
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

- (void)addGame {
    NSMutableArray<UTType *> *types = [NSMutableArray arrayWithObject:UTTypeZIP];
    UTType *vpkType = [UTType typeWithFilenameExtension:@"vpk"];
    if (vpkType != nil) {
        [types addObject:vpkType];
    }
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *archiveURL = urls.firstObject;
    if (archiveURL == nil) {
        return;
    }
    self.addGameButton.enabled = NO;
    self.detailsLabel.text = @"Installing selected archive transactionally...";
    __weak VitaViewController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const BOOL securityScoped = [archiveURL startAccessingSecurityScopedResource];
        vita3k::ios::GameInstallResult result;
        const char *path = archiveURL.fileSystemRepresentation;
        if (path != nullptr) {
            result = vita3k::ios::install_game_archive(std::filesystem::path(path));
        } else {
            result.attempted = true;
            result.detail = "The selected document did not expose a filesystem path.";
        }
        if (securityScoped) {
            [archiveURL stopAccessingSecurityScopedResource];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            VitaViewController *strongSelf = weakSelf;
            if (strongSelf == nil) {
                return;
            }
            strongSelf.addGameButton.enabled = YES;
            vita3k::ios::log_message(result.success ? "INFO" : "ERROR", result.detail);
            [strongSelf refreshStatus];
            NSString *message = [NSString stringWithUTF8String:result.detail.c_str()];
            UIAlertController *alert = [UIAlertController
                alertControllerWithTitle:(result.success ? @"Game Added" : @"Install Failed")
                                 message:message
                          preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                      style:UIAlertActionStyleDefault
                                                    handler:nil]];
            [strongSelf presentViewController:alert animated:YES completion:nil];
        });
    });
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    self.addGameButton.enabled = YES;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self.inputAdapter submitTouches:touches phase:VitaTouchPhaseBegan];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self.inputAdapter submitTouches:touches phase:VitaTouchPhaseMoved];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self.inputAdapter submitTouches:touches phase:VitaTouchPhaseEnded];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self.inputAdapter submitTouches:touches phase:VitaTouchPhaseCancelled];
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
