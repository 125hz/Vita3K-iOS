#pragma once

#import <UIKit/UIKit.h>

typedef NS_ENUM(NSUInteger, VitaTouchPhase) {
    VitaTouchPhaseBegan,
    VitaTouchPhaseMoved,
    VitaTouchPhaseEnded,
    VitaTouchPhaseCancelled
};

typedef void (^VitaInputCompletion)(void);

@interface VitaInputAdapter : NSObject

- (instancetype)initWithView:(UIView *)view completion:(VitaInputCompletion)completion;
- (void)attachTouchSurface;
- (void)submitTouches:(NSSet<UITouch *> *)touches phase:(VitaTouchPhase)phase;

@end
