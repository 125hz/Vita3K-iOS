#import <vita3k_ios/IOSMetalRenderer.h>
#import <dispatch/dispatch.h>

#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/IOSLogger.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace {

bool drawable_extent(CGSize size, std::uint32_t &width, std::uint32_t &height) {
    if (!std::isfinite(size.width) || !std::isfinite(size.height) ||
        size.width < 1.0 || size.height < 1.0 ||
        size.width > std::numeric_limits<std::uint32_t>::max() ||
        size.height > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    width = static_cast<std::uint32_t>(size.width);
    height = static_cast<std::uint32_t>(size.height);
    return true;
}

} // namespace

@interface VitaMetalRenderer ()
@property(nonatomic, weak) MTKView *view;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, copy) VitaMetalFrameCompletion completion;
@end

@implementation VitaMetalRenderer

- (instancetype)initWithView:(MTKView *)view
                  completion:(VitaMetalFrameCompletion)completion {
    self = [super init];
    if (self) {
        _view = view;
        _commandQueue = [view.device newCommandQueue];
        _completion = [completion copy];
        view.delegate = self;
    }
    return self;
}

- (void)requestFirstFrame {
    if (self.commandQueue == nil) {
        vita3k::ios::log_message("ERROR", "Metal command queue creation failed.");
        return;
    }
    [self.view setNeedsDisplay];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!drawable_extent(size, width, height)) {
        return;
    }
    std::string error;
    if (!vita3k::ios::attach_host_display(width, height, error)) {
        vita3k::ios::log_message("ERROR", error);
    }
}

- (void)drawInMTKView:(MTKView *)view {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!drawable_extent(view.drawableSize, width, height)) {
        return;
    }

    std::string error;
    const auto frame = vita3k::ios::acquire_host_display_frame(width, height, error);
    if (!frame) {
        if (!error.empty()) {
            vita3k::ios::log_message("ERROR", error);
        }
        return;
    }

    MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
    if (pass == nil || drawable == nil || commandBuffer == nil) {
        std::string completionError;
        vita3k::ios::complete_host_display_frame(frame->identifier, false,
            "Metal drawable or command buffer was unavailable.", completionError);
        vita3k::ios::log_message("ERROR", completionError.empty()
            ? "Metal drawable or command buffer was unavailable." : completionError);
        [view setNeedsDisplay];
        return;
    }

    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(
        frame->red, frame->green, frame->blue, frame->alpha);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
        std::string completionError;
        vita3k::ios::complete_host_display_frame(frame->identifier, false,
            "Metal render-command encoder creation failed.", completionError);
        vita3k::ios::log_message("ERROR", completionError.empty()
            ? "Metal render-command encoder creation failed." : completionError);
        [view setNeedsDisplay];
        return;
    }
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];

    const auto identifier = frame->identifier;
    __weak VitaMetalRenderer *weakSelf = self;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        const bool presented = completed.status == MTLCommandBufferStatusCompleted;
        std::string detail;
        if (!presented) {
            NSString *description = completed.error.localizedDescription;
            const char *utf8Description = description.UTF8String;
            detail = utf8Description == nullptr ? "Metal command buffer did not complete."
                                                : utf8Description;
        }
        std::string completionError;
        if (!vita3k::ios::complete_host_display_frame(identifier, presented,
                std::move(detail), completionError)) {
            vita3k::ios::log_message("ERROR", completionError);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            VitaMetalRenderer *renderer = weakSelf;
            if (renderer == nil) {
                return;
            }
            if (!presented) {
                [renderer.view setNeedsDisplay];
            }
            if (renderer.completion != nil) {
                renderer.completion();
            }
        });
    }];
    [commandBuffer commit];
}

@end
