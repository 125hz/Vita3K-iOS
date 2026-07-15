#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vita3k_ios/CoreBridge.h>
#import <vita3k_ios/IOSInputAdapter.h>
#include <vita3k_ios/IOSLogger.h>
#import <vita3k_ios/IOSMetalRenderer.h>

static NSString *const VitaPreferPatchKey = @"VitaPreferPatch";
static NSString *const VitaShowDiagnosticsKey = @"VitaShowDiagnostics";

@interface VitaLibraryViewController : UITableViewController
@property(nonatomic, copy) NSArray<NSDictionary<NSString *, id> *> *titles;
@property(nonatomic, copy) void (^selectionHandler)(NSString *titleID);
@end

@implementation VitaLibraryViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Game Library";
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                              target:self
                              action:@selector(closeLibrary)];
}

- (void)closeLibrary {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    (void)section;
    return (NSInteger)self.titles.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const identifier = @"InstalledTitle";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:identifier];
    if (cell == nil) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                      reuseIdentifier:identifier];
    }
    NSDictionary<NSString *, id> *title = self.titles[(NSUInteger)indexPath.row];
    cell.textLabel.text = title[@"title"];
    NSString *source = [title[@"patchEboot"] boolValue] ? @"patch eboot ready" :
        ([title[@"baseEboot"] boolValue] ? @"base eboot ready" : @"eboot missing");
    cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ - %@", title[@"id"], source];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString *titleID = self.titles[(NSUInteger)indexPath.row][@"id"];
    void (^handler)(NSString *) = self.selectionHandler;
    [self dismissViewControllerAnimated:YES completion:^{
        if (handler != nil) {
            handler(titleID);
        }
    }];
}

@end

@interface VitaSettingsViewController : UITableViewController
@end

@implementation VitaSettingsViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Settings";
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                              target:self
                              action:@selector(closeSettings)];
}

- (void)closeSettings {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    (void)section;
    return 2;
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    NSString *identifier = indexPath.row == 0 ? @"PreferPatch" : @"ShowDiagnostics";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:identifier];
    if (cell == nil) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                      reuseIdentifier:identifier];
    }
    const BOOL preferPatch = indexPath.row == 0;
    cell.textLabel.text = preferPatch ? @"Prefer Installed Patch" : @"Show Diagnostics";
    cell.detailTextLabel.text = preferPatch
        ? @"Prepare patch/eboot.bin before the base app executable."
        : @"Show the detailed core and loader report on the main screen.";
    UISwitch *toggle = [[UISwitch alloc] init];
    toggle.tag = indexPath.row;
    NSString *key = preferPatch ? VitaPreferPatchKey : VitaShowDiagnosticsKey;
    toggle.on = [[NSUserDefaults standardUserDefaults] boolForKey:key];
    [toggle addTarget:self action:@selector(settingChanged:)
     forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
}

- (void)settingChanged:(UISwitch *)sender {
    NSString *key = sender.tag == 0 ? VitaPreferPatchKey : VitaShowDiagnosticsKey;
    [[NSUserDefaults standardUserDefaults] setBool:sender.isOn forKey:key];
}

@end

@interface VitaViewController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) UILabel *detailsLabel;
@property(nonatomic, strong) UIButton *addGameButton;
@property(nonatomic, strong) UIButton *libraryButton;
@property(nonatomic, strong) UIButton *bootButton;
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

- (UIButton *)buttonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    UILabel *title = [[UILabel alloc] init];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.text = @"Vita3K iOS - Core Milestone 38";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.textAlignment = NSTextAlignmentCenter;

    self.detailsLabel = [[UILabel alloc] init];
    self.detailsLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.detailsLabel.textColor = [UIColor colorWithWhite:0.78 alpha:1.0];
    self.detailsLabel.font = [UIFont monospacedSystemFontOfSize:14.0 weight:UIFontWeightRegular];
    self.detailsLabel.numberOfLines = 0;
    self.detailsLabel.textAlignment = NSTextAlignmentLeft;

    self.addGameButton = [self buttonWithTitle:@"Add Game ZIP/VPK" action:@selector(addGame)];
    self.libraryButton = [self buttonWithTitle:@"Game Library" action:@selector(openLibrary)];
    self.bootButton = [self buttonWithTitle:@"Attempt Boot (65536 Instructions)"
                                    action:@selector(attemptBoot)];
    self.bootButton.enabled = NO;
    UIButton *settingsButton = [self buttonWithTitle:@"Settings" action:@selector(openSettings)];
    UIButton *rescanButton = [self buttonWithTitle:@"Rescan Imports" action:@selector(rescanImports)];

    UIStackView *primaryButtons = [[UIStackView alloc]
        initWithArrangedSubviews:@[self.addGameButton, self.libraryButton]];
    primaryButtons.axis = UILayoutConstraintAxisHorizontal;
    primaryButtons.distribution = UIStackViewDistributionFillEqually;
    primaryButtons.spacing = 24.0;
    UIStackView *secondaryButtons = [[UIStackView alloc]
        initWithArrangedSubviews:@[settingsButton, rescanButton]];
    secondaryButtons.axis = UILayoutConstraintAxisHorizontal;
    secondaryButtons.distribution = UIStackViewDistributionFillEqually;
    secondaryButtons.spacing = 24.0;

    UIStackView *stack = [[UIStackView alloc]
        initWithArrangedSubviews:@[
            title, self.detailsLabel, primaryButtons, self.bootButton, secondaryButtons
        ]];
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
    [self refreshStatus];
}

- (void)refreshStatus {
    const auto status = vita3k::ios::query_core_status();
    self.detailsLabel.text = [NSString stringWithUTF8String:status.summary.c_str()];
    self.bootButton.enabled = status.selected_boot_available;
    self.detailsLabel.hidden = ![[NSUserDefaults standardUserDefaults]
        boolForKey:VitaShowDiagnosticsKey];
}

- (void)rescanImports {
    const auto status = vita3k::ios::rescan_imports();
    vita3k::ios::log_message("INFO", status.summary);
    [self refreshStatus];
}

- (void)openLibrary {
    const auto status = vita3k::ios::query_core_status();
    if (status.installed_titles.empty()) {
        UIAlertController *alert = [UIAlertController
            alertControllerWithTitle:@"No Installed Games"
                             message:@"Use Add Game ZIP/VPK first."
                      preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                  style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }
    NSMutableArray<NSDictionary<NSString *, id> *> *titles = [NSMutableArray array];
    for (const auto &item : status.installed_titles) {
        [titles addObject:@{
            @"id": [NSString stringWithUTF8String:item.title_id.c_str()],
            @"title": [NSString stringWithUTF8String:item.title.c_str()],
            @"baseEboot": @(item.base_eboot_present),
            @"patchEboot": @(item.patch_eboot_present)
        }];
    }
    VitaLibraryViewController *library = [[VitaLibraryViewController alloc]
        initWithStyle:UITableViewStyleInsetGrouped];
    library.titles = titles;
    __weak VitaViewController *weakSelf = self;
    library.selectionHandler = ^(NSString *titleID) {
        [weakSelf prepareTitle:titleID];
    };
    UINavigationController *navigation = [[UINavigationController alloc]
        initWithRootViewController:library];
    [self presentViewController:navigation animated:YES completion:nil];
}

- (void)prepareTitle:(NSString *)titleID {
    self.libraryButton.enabled = NO;
    self.detailsLabel.hidden = NO;
    self.detailsLabel.text = @"Inspecting selected eboot.bin and preparing guest memory...";
    const BOOL preferPatch = [[NSUserDefaults standardUserDefaults] boolForKey:VitaPreferPatchKey];
    __weak VitaViewController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const char *identifier = titleID.UTF8String;
        const auto result = vita3k::ios::prepare_installed_title(
            identifier == nullptr ? std::string{} : std::string(identifier), preferPatch);
        dispatch_async(dispatch_get_main_queue(), ^{
            VitaViewController *strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            strongSelf.libraryButton.enabled = YES;
            vita3k::ios::log_message(result.loaded ? "INFO" : "WARN", result.detail);
            [strongSelf refreshStatus];
            NSString *message = [NSString stringWithUTF8String:result.detail.c_str()];
            UIAlertController *alert = [UIAlertController
                alertControllerWithTitle:(result.loaded ? @"Executable Prepared" : @"Preparation Stopped")
                                 message:message
                          preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                      style:UIAlertActionStyleDefault handler:nil]];
            [strongSelf presentViewController:alert animated:YES completion:nil];
        });
    });
}

- (void)attemptBoot {
    UIAlertController *confirmation = [UIAlertController
        alertControllerWithTitle:@"Controlled Interpreter Attempt"
                         message:@"Run the prepared module once with a hard ceiling of 65536 interpreted instructions? JIT is not used, and execution stops at the first unsupported instruction, memory fault, unimplemented HLE call, or detected diagnostic boundary."
                  preferredStyle:UIAlertControllerStyleAlert];
    [confirmation addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                      style:UIAlertActionStyleCancel handler:nil]];
    __weak VitaViewController *weakSelf = self;
    [confirmation addAction:[UIAlertAction actionWithTitle:@"Run Once"
                                                      style:UIAlertActionStyleDestructive
                                                    handler:^(UIAlertAction *action) {
        (void)action;
        [weakSelf runPreparedTitle];
    }]];
    [self presentViewController:confirmation animated:YES completion:nil];
}

- (void)runPreparedTitle {
    self.bootButton.enabled = NO;
    self.detailsLabel.hidden = NO;
    self.detailsLabel.text = @"Running the bounded interpreter (maximum 65536 instructions)...";
    __weak VitaViewController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const auto result = vita3k::ios::attempt_prepared_title_boot(65536);
        dispatch_async(dispatch_get_main_queue(), ^{
            VitaViewController *strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            vita3k::ios::log_message(
                (result.exited || result.returned) ? "INFO" : "WARN", result.detail);
            [strongSelf refreshStatus];
            NSString *message = [NSString stringWithUTF8String:result.detail.c_str()];
            UIAlertController *alert = [UIAlertController
                alertControllerWithTitle:(result.exited || result.returned
                    ? @"Module Completed" : @"Boot Boundary Captured")
                                 message:message
                          preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                      style:UIAlertActionStyleDefault handler:nil]];
            [strongSelf presentViewController:alert animated:YES completion:nil];
        });
    });
}

- (void)openSettings {
    VitaSettingsViewController *settings = [[VitaSettingsViewController alloc]
        initWithStyle:UITableViewStyleInsetGrouped];
    UINavigationController *navigation = [[UINavigationController alloc]
        initWithRootViewController:settings];
    [self presentViewController:navigation animated:YES completion:nil];
}

- (void)addGame {
    NSMutableArray<UTType *> *types = [NSMutableArray arrayWithObject:UTTypeZIP];
    UTType *vpkType = [UTType typeWithFilenameExtension:@"vpk"];
    if (vpkType != nil)
        [types addObject:vpkType];
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
    if (archiveURL == nil)
        return;
    self.addGameButton.enabled = NO;
    self.detailsLabel.hidden = NO;
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
        if (securityScoped)
            [archiveURL stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            VitaViewController *strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            strongSelf.addGameButton.enabled = YES;
            vita3k::ios::log_message(result.success ? "INFO" : "ERROR", result.detail);
            [strongSelf refreshStatus];
            NSString *message = [NSString stringWithUTF8String:result.detail.c_str()];
            UIAlertController *alert = [UIAlertController
                alertControllerWithTitle:(result.success ? @"Game Added" : @"Install Failed")
                                 message:message
                          preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                      style:UIAlertActionStyleDefault handler:nil]];
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
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        VitaPreferPatchKey: @YES,
        VitaShowDiagnosticsKey: @YES
    }];
    vita3k::ios::initialize_logging();
    const auto coreStatus = vita3k::ios::initialize_core(
        vita3k::ios::log_file_path().parent_path());
    vita3k::ios::log_message("INFO", coreStatus.summary);

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
