#pragma once

#import <MetalKit/MetalKit.h>

typedef void (^VitaMetalFrameCompletion)(void);

@interface VitaMetalRenderer : NSObject <MTKViewDelegate>

- (instancetype)initWithView:(MTKView *)view
                  completion:(VitaMetalFrameCompletion)completion;
- (void)requestFirstFrame;

@end
