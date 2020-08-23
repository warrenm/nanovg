
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "nanovg.h"
#define NANOVG_METAL_IMPLEMENTATION
#include "nanovg_metal.h"
#include "demo.h"
#include "perf.h"

enum {
    kVK_ANSI_S = 0x01,
    kVK_ANSI_P = 0x23,
    kVK_Space  = 0x31,
    kVK_Escape = 0x35
};

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end

@interface ViewController : NSViewController <MTKViewDelegate>
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, assign) DemoData data;
@property (nonatomic, assign) NVGcontext *vg;
@property (nonatomic, assign) PerfGraph fps, cpuGraph, gpuGraph;
@property (nonatomic, assign) double prevt;
@property (nonatomic, assign) int blowup;
@property (nonatomic, assign) int screenshot;
@property (nonatomic, assign) int premult;
@end

@implementation ViewController

#pragma mark - View Lifecycle

- (void)viewDidAppear {
    [super viewDidAppear];
    
    self.device = MTLCreateSystemDefaultDevice();
    self.commandQueue = [self.device newCommandQueue];
    
    NSLog(@"Metal Device: %@", self.device);
    
    MTKView *mtkView = (MTKView *)self.view;
    mtkView.device = self.device;
    mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    mtkView.depthStencilPixelFormat = MTLPixelFormatStencil8;
    mtkView.delegate = self;

    _prevt = CACurrentMediaTime();
    initGraph(&_fps, GRAPH_RENDER_FPS, "Frame Rate");
    initGraph(&_cpuGraph, GRAPH_RENDER_MS, "CPU Time");
    initGraph(&_gpuGraph, GRAPH_RENDER_MS, "GPU Time");
    _vg = nvgCreateMetal(self.device, self.commandQueue, NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
    loadDemoData(_vg, &_data);
    
    [self.view.window makeFirstResponder:self];
}

- (void)viewDidDisappear {
    [super viewDidDisappear];
    freeDemoData(_vg, &_data);
}

#pragma mark - Input

- (BOOL)becomeFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    if (event.keyCode == kVK_Escape) {
        [self.view.window close];
    } else if (event.keyCode == kVK_Space) {
        _blowup = !_blowup;
    } else if (event.keyCode == kVK_ANSI_S) {
        _screenshot = 1;
    } else if (event.keyCode == kVK_ANSI_P) {
        _premult = !_premult;
    }
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView *)view {
    double t = CACurrentMediaTime();
    double dt = t - _prevt;
    _prevt = t;
    updateGraph(&_fps, dt);

    NSPoint asyncMousePos = [view.window mouseLocationOutsideOfEventStream];
    asyncMousePos = [view convertPoint:asyncMousePos fromView:nil];
    int mx = asyncMousePos.x;
    int my = view.bounds.size.height - asyncMousePos.y;
    
    int winWidth = view.bounds.size.width;
    int winHeight = view.bounds.size.height;
    
    int fbWidth = (int)view.drawableSize.width;
    int fbHeight = (int)view.drawableSize.height;

    // Calculate point size to backing ratio
    float pxRatio = (float)fbWidth / (float)winWidth;

    MTLRenderPassDescriptor *renderPass = view.currentRenderPassDescriptor;
    if (renderPass == nil) {
        return;
    }
    
    if (_premult) {
        renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.3f, 0.3f, 0.32f, 1.0f);
    }

    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> renderCommandEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];

    MTLViewport viewport = { 0.0, 0.0, fbWidth, fbHeight, 0.0, 1.0 };
    [renderCommandEncoder setViewport:viewport];
    
    nvgBeginFrameMetal(_vg, commandBuffer, renderCommandEncoder);
    
    nvgBeginFrame(_vg, winWidth, winHeight, pxRatio);
    
    renderDemo(_vg, mx, my, winWidth, winHeight, t, _blowup, &_data);

    renderGraph(_vg, 5, 5, &_fps);
    renderGraph(_vg, 5 + 200 + 5, 5, &_cpuGraph);
    renderGraph(_vg, 5 + 200 + 5 + 200 + 5, 5, &_gpuGraph);

    nvgEndFrame(_vg);
    
    [renderCommandEncoder endEncoding];
    
    [commandBuffer presentDrawable:view.currentDrawable];
    
    if (@available(macOS 10.15, *)) {
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedCommandBuffer) {
            NSTimeInterval gpuExecutionTime = completedCommandBuffer.GPUEndTime - completedCommandBuffer.GPUStartTime;
            dispatch_async(dispatch_get_main_queue(), ^{
                updateGraph(&self->_gpuGraph, gpuExecutionTime);
            });
        }];
    }

    [commandBuffer commit];

    double tp = CACurrentMediaTime();
    updateGraph(&_cpuGraph, tp - t);

    if (_screenshot) {
        _screenshot = 0;
        [commandBuffer waitUntilCompleted];
        saveScreenShot(fbWidth, fbHeight, _premult, "dump.png");
    }
}

@end

int main(int argc, const char * argv[]) {
    return NSApplicationMain(argc, argv);
}
