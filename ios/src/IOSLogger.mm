#include <vita3k_ios/IOSLogger.h>

#import <Foundation/Foundation.h>
#import <os/log.h>

#include <fstream>
#include <mutex>
#include <string>

namespace vita3k::ios {
namespace {

std::mutex log_mutex;
os_log_t app_log = nullptr;

std::string timestamp() {
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.dateFormat = @"yyyy-MM-dd'T'HH:mm:ss.SSSZZZZZ";
    return formatter.stringFromDate(NSDate.date).UTF8String;
}

} // namespace

std::filesystem::path log_file_path() {
    NSArray<NSURL *> *urls = [[NSFileManager defaultManager]
        URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask];
    NSURL *documents = urls.firstObject;
    return std::filesystem::path(documents.fileSystemRepresentation) / "vita3k.log";
}

void initialize_logging() {
    app_log = os_log_create("org.vita3k.experimental.ios", "app");
    log_message("INFO", "Logging initialized");
}

void log_message(std::string_view level, std::string_view message) {
    if (app_log == nullptr) {
        app_log = os_log_create("org.vita3k.experimental.ios", "app");
    }

    const std::string level_copy(level);
    const std::string message_copy(message);
    os_log_with_type(app_log, OS_LOG_TYPE_DEFAULT, "[%{public}s] %{public}s",
        level_copy.c_str(), message_copy.c_str());

    std::lock_guard lock(log_mutex);
    std::ofstream stream(log_file_path(), std::ios::app);
    stream << timestamp() << " [" << level << "] " << message << '\n';
}

} // namespace vita3k::ios
