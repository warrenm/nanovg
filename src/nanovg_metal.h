
#ifndef NANOVG_METAL_H
#define NANOVG_METAL_H

#ifdef __cplusplus
extern "C" {
#endif

#pragma mark - Public Interface

enum NVGcreateFlags {
    // Flag indicating if geometry based anti-aliasing is used (may not be needed when using MSAA).
    NVG_ANTIALIAS 		= 1 << 0,
    // Flag indicating if strokes should be drawn using stencil buffer. The rendering will be a little
    // slower, but path overlaps (i.e. self-intersecting or sharp turns) will be drawn just once.
    NVG_STENCIL_STROKES	= 1 << 1,
    // Flag indicating that additional debug checks are done (not implemented in this backend).
    NVG_DEBUG             = 1<<2,
};

#if defined NANOVG_METAL_IMPLEMENTATION
#define NANOVG_METAL 1
#endif

#if defined NANOVG_METAL

NS_ASSUME_NONNULL_BEGIN

NVGcontext *_Nullable nvgCreateMetal(id<MTLDevice> device, id<MTLCommandQueue> commandQueue, int flags);
void nvgDeleteMetal(NVGcontext *ctx);
void nvgBeginFrameMetal(NVGcontext *ctx, id<MTLCommandBuffer> commandBuffer, id<MTLRenderCommandEncoder> rce);

NS_ASSUME_NONNULL_END

#endif

#ifdef __cplusplus
}
#endif

#endif /* NANOVG_METAL_H */

#pragma mark - Implementation

#ifdef NANOVG_METAL_IMPLEMENTATION

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include "nanovg.h"

NS_ASSUME_NONNULL_BEGIN

#pragma mark - Internal Structures

enum NVGMTLCallType {
    NVGMTL_NONE = 0,
    NVGMTL_FILL,
    NVGMTL_CONVEXFILL,
    NVGMTL_STROKE,
    NVGMTL_TRIANGLES,
};

enum NVGMTLShaderID {
    NVGMTL_SHADER_FILLGRAD,
    NVGMTL_SHADER_FILLIMG,
    NVGMTL_SHADER_SIMPLE,
    NVGMTL_SHADER_IMG
};

typedef struct {
    int32_t srcRGBFactor;
    int32_t dstRGBFactor;
    int32_t srcAlphaFactor;
    int32_t dstAlphaFactor;
} NVGMTLBlendState;

typedef struct {
    uint32_t type;
    uint32_t image;
    uint32_t pathOffset;
    uint32_t pathCount;
    uint32_t baseVertex;
    uint32_t triangleCount;
    uint32_t uniformOffset;
    NVGMTLBlendState blendState;
} NVGMTLCall;

typedef struct {
    uint32_t fillOffset;
    uint32_t fillCount;
    uint32_t strokeOffset;
    uint32_t strokeCount;
} NVGMTLPath;

typedef struct {
    float scissorMat[12]; // matrices are actually 3 vec4s
    float paintMat[12];
    struct NVGcolor innerCol;
    struct NVGcolor outerCol;
    float scissorExt[2];
    float scissorScale[2];
    float extent[2];
    float radius;
    float feather;
    float strokeMult;
    float strokeThr;
    uint32_t texType;
    uint32_t type;
} NVGMTLFragmentUniforms;

// Actual fragment uniform size is 176 bytes, but we need at least
// a 256 byte stride between addressable buffer offsets on some GPUs,
// so we use that upper bound here. If fragment uniforms ever become
// larger than 256 byts, this should be adjusted upward to the next power
// of two.
static const int NVGMTLFragmentUniformStride = 256;

#pragma mark - Internal Utility Functions

static void NVGMTLFloat3x4FromElements(float* m3, float* t) {
    m3[0] = t[0];
    m3[1] = t[1];
    m3[2] = 0.0f;
    m3[3] = 0.0f;
    m3[4] = t[2];
    m3[5] = t[3];
    m3[6] = 0.0f;
    m3[7] = 0.0f;
    m3[8] = t[4];
    m3[9] = t[5];
    m3[10] = 1.0f;
    m3[11] = 0.0f;
}

static NVGcolor NVGMTLPremultiplyColor(NVGcolor c) {
    c.r *= c.a;
    c.g *= c.a;
    c.b *= c.a;
    return c;
}

static int NVGMetalBlendFactorForNVGBlendFactor(int factor) {
    switch (factor) {
        case NVG_ZERO:
            return MTLBlendFactorZero;
        case NVG_ONE:
            return MTLBlendFactorOne;
        case NVG_SRC_COLOR:
            return MTLBlendFactorSourceColor;
        case NVG_ONE_MINUS_SRC_COLOR:
            return MTLBlendFactorOneMinusSourceColor;
        case NVG_DST_COLOR:
            return MTLBlendFactorDestinationColor;
        case NVG_ONE_MINUS_DST_COLOR:
            return MTLBlendFactorOneMinusDestinationColor;
        case NVG_SRC_ALPHA:
            return MTLBlendFactorSourceAlpha;
        case NVG_ONE_MINUS_SRC_ALPHA:
            return MTLBlendFactorOneMinusSourceAlpha;
        case NVG_DST_ALPHA:
            return MTLBlendFactorDestinationAlpha;
        case NVG_ONE_MINUS_DST_ALPHA:
            return MTLBlendFactorOneMinusDestinationAlpha;
        case NVG_SRC_ALPHA_SATURATE:
            return MTLBlendFactorSourceAlphaSaturated;
        default:
            return -1;
    }
}

static NVGMTLBlendState NVGMetalBlendStateFromCompOpState(NVGcompositeOperationState op)
{
    NVGMTLBlendState blendState;
    blendState.srcRGBFactor = NVGMetalBlendFactorForNVGBlendFactor(op.srcRGB);
    blendState.dstRGBFactor = NVGMetalBlendFactorForNVGBlendFactor(op.dstRGB);
    blendState.srcAlphaFactor = NVGMetalBlendFactorForNVGBlendFactor(op.srcAlpha);
    blendState.dstAlphaFactor = NVGMetalBlendFactorForNVGBlendFactor(op.dstAlpha);
    return blendState;
}

#pragma mark - Backend Implementation

@interface NVGMTLTexture : NSObject
@property (nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic, assign) NSUInteger identifier;
@property (nonatomic, assign) UInt32 type;
@property (nonatomic, assign) UInt32 flags;
@end

@implementation NVGMTLTexture
@end

@interface NVGMTLContext : NSObject
@property (class, readonly, strong) NSMutableDictionary<NSNumber *, NVGMTLContext *> *contextRegistry;
@property (class, assign) NSUInteger nextContextSlot;

@property (nonatomic, nonnull, readonly) id<MTLDevice> device;
@property (nonatomic, nonnull, readonly) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) NSMutableArray *reusableBufferPool;
@property (nonatomic, strong) id<MTLRenderPipelineState> stencilPopulateRenderPipelineState;
@property (nonatomic, strong) id<MTLRenderPipelineState> fillRenderPipelineState;
@property (nonatomic, strong) id<MTLDepthStencilState> defaultStencilState;
@property (nonatomic, strong) id<MTLDepthStencilState> fillStencilState;
@property (nonatomic, strong) id<MTLDepthStencilState> equalStencilState;
@property (nonatomic, strong) id<MTLDepthStencilState> zeroOnNotEqualStencilState;
@property (nonatomic, strong) id<MTLDepthStencilState> incrementOnEqualStencilState;
@property (nonatomic, strong) id<MTLDepthStencilState> alwaysZeroStencilState;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NVGMTLTexture *> *textures;
@property (nonatomic, assign) NSInteger nextTextureID;

@property (nonatomic, nullable) id<MTLCommandBuffer> currentCommandBuffer;
@property (nonatomic, nullable) id<MTLRenderCommandEncoder> currentRenderCommandEncoder;
@property (nonatomic, strong) id<MTLBuffer> currentVertexBuffer;

@property (nonatomic, weak) id<MTLRenderPipelineState> currentRenderPipelineState;
@property (nonatomic, weak) id<MTLDepthStencilState> currentDepthStencilState;
@property (nonatomic, weak) id<MTLTexture> currentTexture;

@property (nonatomic, assign) simd_float2 viewportSize;
@property (nonatomic, assign) int flags;
@property (nonatomic, assign) NVGMTLCall *calls;
@property (nonatomic, assign) int ccalls;
@property (nonatomic, assign) int ncalls;
@property (nonatomic, assign) NVGMTLPath *paths;
@property (nonatomic, assign) int cpaths;
@property (nonatomic, assign) int npaths;
@property (nonatomic, assign) struct NVGvertex *verts;
@property (nonatomic, assign) int cverts;
@property (nonatomic, assign) int nverts;
@property (nonatomic, assign) unsigned char *uniforms;
@property (nonatomic, assign) int cuniforms;
@property (nonatomic, assign) int nuniforms;
@end

static NSMutableDictionary<NSNumber *, NVGMTLContext *> *NVGMTLContextRegistry = nil;
static NSUInteger NVGMTLNextContextSlot = 1;

@implementation NVGMTLContext

+ (void)initialize {
    NVGMTLContextRegistry = [[NSMutableDictionary alloc] init];
}

+ (NSMutableDictionary *)contextRegistry {
    return NVGMTLContextRegistry;
}

+ (NSUInteger)nextContextSlot {
    return NVGMTLNextContextSlot;
}

+ (void)setNextContextSlot:(NSUInteger)nextContextSlot {
    NVGMTLNextContextSlot = nextContextSlot;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device
                  commandQueue:(id<MTLCommandQueue>)commandQueue
                         flags:(int)flags
{
    if ((self = [super init])) {
        _device = device;
        _commandQueue = commandQueue;
        _textures = [NSMutableDictionary new];
        _reusableBufferPool = [NSMutableArray new];
        _flags = flags;
        [self _makePipelines];
    }
    return self;
}

- (void)_makePipelines {
    MTLRenderPipelineDescriptor *renderPipelineDescriptor = [MTLRenderPipelineDescriptor new];

    id<MTLLibrary> library = [_device newDefaultLibrary];
    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragment_main"];

    renderPipelineDescriptor.vertexFunction = vertexFunction;
    renderPipelineDescriptor.fragmentFunction = fragmentFunction;

    MTLVertexDescriptor *vertexDescriptor = [MTLVertexDescriptor new];
    vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2; // position
    vertexDescriptor.attributes[0].bufferIndex = 0;
    vertexDescriptor.attributes[0].offset = 0;
    vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2; // texcoord
    vertexDescriptor.attributes[1].bufferIndex = 0;
    vertexDescriptor.attributes[1].offset = sizeof(float) * 2;
    vertexDescriptor.layouts[0].stride = sizeof(float) * 4;

    renderPipelineDescriptor.vertexDescriptor = vertexDescriptor;
    
    renderPipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    renderPipelineDescriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    renderPipelineDescriptor.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    
    NSError *error = nil;
    self.stencilPopulateRenderPipelineState = [_device newRenderPipelineStateWithDescriptor:renderPipelineDescriptor
                                                                                  error:&error];
    if (_stencilPopulateRenderPipelineState == nil) {
        NSLog(@"Failed to create render pipeline state: %@", error);
    }
    
    renderPipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
    renderPipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    renderPipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    renderPipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    renderPipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    renderPipelineDescriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    
    self.fillRenderPipelineState = [_device newRenderPipelineStateWithDescriptor:renderPipelineDescriptor error:&error];
    if (_fillRenderPipelineState == nil) {
        NSLog(@"Failed to create render pipeline state: %@", error);
    }

    MTLDepthStencilDescriptor *stencilDescriptor = [MTLDepthStencilDescriptor new];
    self.defaultStencilState = [_device newDepthStencilStateWithDescriptor:stencilDescriptor];

    stencilDescriptor = [MTLDepthStencilDescriptor new];
    stencilDescriptor.frontFaceStencil.depthStencilPassOperation = MTLStencilOperationIncrementWrap;
    stencilDescriptor.backFaceStencil.depthStencilPassOperation = MTLStencilOperationDecrementWrap;
    self.fillStencilState = [_device newDepthStencilStateWithDescriptor:stencilDescriptor];
    
    stencilDescriptor = [MTLDepthStencilDescriptor new];
    stencilDescriptor.frontFaceStencil.stencilCompareFunction = MTLCompareFunctionEqual;
    stencilDescriptor.backFaceStencil.stencilCompareFunction = MTLCompareFunctionEqual;
    self.equalStencilState = [_device newDepthStencilStateWithDescriptor:stencilDescriptor];

    stencilDescriptor = [MTLDepthStencilDescriptor new];
    stencilDescriptor.frontFaceStencil.stencilCompareFunction = MTLCompareFunctionNotEqual;
    stencilDescriptor.frontFaceStencil.stencilFailureOperation = MTLStencilOperationZero;
    stencilDescriptor.frontFaceStencil.depthFailureOperation = MTLStencilOperationZero;
    stencilDescriptor.frontFaceStencil.depthStencilPassOperation = MTLStencilOperationZero;
    stencilDescriptor.backFaceStencil = stencilDescriptor.frontFaceStencil;
    self.zeroOnNotEqualStencilState = [_device newDepthStencilStateWithDescriptor:stencilDescriptor];

    stencilDescriptor = [MTLDepthStencilDescriptor new];
    stencilDescriptor.frontFaceStencil.stencilCompareFunction = MTLCompareFunctionEqual;
    stencilDescriptor.frontFaceStencil.depthStencilPassOperation = MTLStencilOperationIncrementWrap;
    stencilDescriptor.backFaceStencil = stencilDescriptor.frontFaceStencil;
    self.incrementOnEqualStencilState = [_device newDepthStencilStateWithDescriptor:stencilDescriptor];
    
    stencilDescriptor = [MTLDepthStencilDescriptor new];
    stencilDescriptor.frontFaceStencil.stencilFailureOperation = MTLStencilOperationZero;
    stencilDescriptor.frontFaceStencil.depthFailureOperation = MTLStencilOperationZero;
    stencilDescriptor.frontFaceStencil.depthStencilPassOperation = MTLStencilOperationZero;
    stencilDescriptor.backFaceStencil = stencilDescriptor.frontFaceStencil;
    self.alwaysZeroStencilState = [_device newDepthStencilStateWithDescriptor:stencilDescriptor];
}

- (void)dealloc
{
    free(_paths);
    free(_verts);
    free(_uniforms);
    free(_calls);
}

#pragma mark - Backend Interface

- (void)renderCancel {
    _nverts = 0;
    _npaths = 0;
    _ncalls = 0;
    _nuniforms = 0;
}

- (void)renderFlush
{
    if (_ncalls > 0) {
        id<MTLRenderCommandEncoder> renderCommandEncoder = self.currentRenderCommandEncoder;
        
        id<MTLBuffer> vertexBuffer = [self _dequeueReusableBufferOfLength:_nverts * sizeof(NVGvertex)];
        memcpy(vertexBuffer.contents, _verts, _nverts * sizeof(NVGvertex));
        
        self.currentVertexBuffer = vertexBuffer;
        
        [renderCommandEncoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];

        [renderCommandEncoder setVertexBytes:&_viewportSize length:sizeof(simd_float2) atIndex:1];

        id<MTLBuffer> uniformBuffer = [self _dequeueReusableBufferOfLength:_nuniforms * NVGMTLFragmentUniformStride];
        memcpy(uniformBuffer.contents, _uniforms, _nuniforms * NVGMTLFragmentUniformStride);

        [renderCommandEncoder setFragmentBuffer:uniformBuffer offset:0 atIndex:0];

        [renderCommandEncoder setCullMode:MTLCullModeBack];
        [renderCommandEncoder setFrontFacingWinding:MTLWindingCounterClockwise];

        [self _setRenderPipelineState:self.fillRenderPipelineState];
        [self _setDepthStencilState:self.defaultStencilState];

        for (int i = 0; i < _ncalls; ++i) {
            NVGMTLCall *call = &_calls[i];
            switch (call->type) {
                case NVGMTL_FILL:
                    [self _drawFill:call];
                    break;
                case NVGMTL_CONVEXFILL:
                    [self _drawConvexFill:call];
                    break;
                case NVGMTL_STROKE:
                    [self _drawStroke:call];
                    break;
                case NVGMTL_TRIANGLES:
                    [self _drawTriangles:call];
                    break;
                default:
                    break;
            }
        }
        
        __weak id weakSelf = self;
        [self.currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            [weakSelf _enqueueReusableBuffer:vertexBuffer];
            [weakSelf _enqueueReusableBuffer:uniformBuffer];
        }];
    }

    _nverts = 0;
    _npaths = 0;
    _ncalls = 0;
    _nuniforms = 0;
}

- (int)makeTextureWithBytes:(const uint8_t *_Nullable)bytes
                       type:(enum NVGtexture)type
                      width:(int)width
                     height:(int)height
                      flags:(int)flags
{
    bool wantMips = (flags & NVG_IMAGE_GENERATE_MIPMAPS) != 0;
    
    MTLPixelFormat format = (type == NVG_TEXTURE_RGBA) ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatR8Unorm;
    int bytesPerRow = (type == NVG_TEXTURE_RGBA) ? (width * 4) : width;

    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                                 width:width
                                                                                                height:height
                                                                                             mipmapped:wantMips];
    textureDescriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [_device newTextureWithDescriptor:textureDescriptor];
    
    if (bytes) {
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:bytes
                   bytesPerRow:bytesPerRow];
    }
    
    /*
    if (imageFlags & NVG_IMAGE_GENERATE_MIPMAPS) {
        if (imageFlags & NVG_IMAGE_NEAREST) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
    } else {
        if (imageFlags & NVG_IMAGE_NEAREST) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
    }
    
    if (imageFlags & NVG_IMAGE_NEAREST) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    
    if (imageFlags & NVG_IMAGE_REPEATX)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    else
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    
    if (imageFlags & NVG_IMAGE_REPEATY)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    else
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    */
    if (bytes && wantMips) {
        id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
        id<MTLBlitCommandEncoder> mipCommandEncoder = [commandBuffer blitCommandEncoder];
        [mipCommandEncoder generateMipmapsForTexture:texture];
        [mipCommandEncoder endEncoding];
        [commandBuffer commit];
    }
    
    _nextTextureID++;
    
    NVGMTLTexture *textureWrapper = [NVGMTLTexture new];
    textureWrapper.texture = texture;
    textureWrapper.identifier = _nextTextureID;
    textureWrapper.type = type;
    textureWrapper.flags = flags;
    
    _textures[@(textureWrapper.identifier)] = textureWrapper;
    
    return (int)textureWrapper.identifier;
}

- (NVGMTLTexture *)textureForIdentifier:(NSUInteger)identifier {
    return _textures[@(identifier)];
}

- (int)getSizeOfTextureForIdentifier:(NSUInteger)identifier width:(int *)outWidth height:(int *)outHeight {
    NVGMTLTexture *textureWrapper = _textures[@(identifier)];
    if (textureWrapper) {
        id<MTLTexture> texture = textureWrapper.texture;
        if (outWidth) {
            *outWidth = (int)texture.width;
        }
        if (outHeight) {
            *outHeight = (int)texture.height;
        }
        return 1;
    }
    return 0; // Doesn't exist
}

- (int)removeTextureForIdentifier:(NSUInteger)identifier {
    NVGMTLTexture *texture = _textures[@(identifier)];
    if (texture) {
        _textures[@(identifier)] = nil;
        return 1;
    }
    return 0; // Previously released or never existed
}

- (int)replaceRegion:(MTLRegion)region ofTexture:(NSUInteger)textureID withBytes:(const uint8_t *)bytes {
    NVGMTLTexture *tex = [self textureForIdentifier:textureID];
    if (tex == nil) {
        return 0;
    }
    
    BOOL isRGBA = (tex.texture.pixelFormat != MTLPixelFormatR8Unorm);
    NSUInteger bytesPerRow = isRGBA ? (tex.texture.width * 4) : tex.texture.width;

    const uint8_t *srcBytes = bytes;
    srcBytes += (bytesPerRow * region.origin.y);
    srcBytes += isRGBA ? (region.origin.x * 4) : region.origin.x;

    [tex.texture replaceRegion:region mipmapLevel:0 withBytes:srcBytes bytesPerRow:bytesPerRow];

    return 1;
}

- (void)setUniformsForOffset:(int)uniformOffset textureIdentifier:(int)textureIdentifier {
    id<MTLRenderCommandEncoder> renderCommandEncoder = self.currentRenderCommandEncoder;
    
    [renderCommandEncoder setFragmentBufferOffset:uniformOffset atIndex:0];

    if (textureIdentifier != 0) {
        NVGMTLTexture *tex = [self textureForIdentifier:textureIdentifier];
        [self _setFragmentTexture:tex.texture];
    } else {
        [self _setFragmentTexture:nil];
    }
}

- (void)setViewportWidth:(float)width height:(float)height {
    _viewportSize.x = width;
    _viewportSize.y = height;
}

- (void)renderFill:(NVGpaint *)paint
compositeOperation:(NVGcompositeOperationState)compositeOperation
           scissor:(NVGscissor *)scissor
            fringe:(float)fringe
            bounds:(const float *)bounds
             paths:(const NVGpath *)paths
            npaths:(int)npaths
{
    NVGMTLCall *call = [self _allocCall];
    if (call == NULL) {
        return;
    }
    
    call->type = NVGMTL_FILL;
    call->triangleCount = 4;
    call->pathOffset = [self _allocPaths:npaths];
    if (call->pathOffset == -1) {
        goto error;
    }
    call->pathCount = npaths;
    call->image = paint->image;
    call->blendState = NVGMetalBlendStateFromCompOpState(compositeOperation);

    if (npaths == 1 && paths[0].convex) {
        call->type = NVGMTL_CONVEXFILL;
        call->triangleCount = 0;
    }

    int maxverts = [self _maxVertCountForPaths:paths pathCount:npaths] + call->triangleCount;
    int offset = [self _allocVerts:maxverts];
    if (offset == -1) {
        goto error;
    }
    
    for (int i = 0; i < npaths; ++i) {
        NVGMTLPath *copy = &_paths[call->pathOffset + i];
        const NVGpath *path = &paths[i];
        memset(copy, 0, sizeof(NVGMTLPath));
        if (path->nfill > 0) {
            copy->fillOffset = offset;
            copy->fillCount = 0;//path->nfill;
            // TODO: Fix up triangle fan verts
            //memcpy(&_verts[offset], path->fill, sizeof(NVGvertex) * path->nfill);
            int triCount = path->nfill - 1;
            for (int j = 0; j < triCount; ++j) {
                memcpy(&_verts[offset + (j * 3) + 0], path->fill + (0), sizeof(NVGvertex));
                memcpy(&_verts[offset + (j * 3) + 1], path->fill + (j), sizeof(NVGvertex));
                memcpy(&_verts[offset + (j * 3) + 2], path->fill + (j+1), sizeof(NVGvertex));
                copy->fillCount += 3;
            }
            offset += copy->fillCount;
        }
        if (path->nstroke > 0) {
            copy->strokeOffset = offset;
            copy->strokeCount = path->nstroke;
            memcpy(&_verts[offset], path->stroke, sizeof(NVGvertex) * path->nstroke);
            offset += path->nstroke;
        }
    }

    if (call->type == NVGMTL_FILL) {
        call->baseVertex = offset;
        NVGvertex *quad;
        quad = &_verts[call->baseVertex];
        quad[0] = (NVGvertex){ bounds[2], bounds[3], 0.5f, 1.0f };
        quad[1] = (NVGvertex){ bounds[2], bounds[1], 0.5f, 1.0f };
        quad[2] = (NVGvertex){ bounds[0], bounds[3], 0.5f, 1.0f };
        quad[3] = (NVGvertex){ bounds[0], bounds[1], 0.5f, 1.0f };
        
        call->uniformOffset = [self _allocFragUniforms: 2];
        if (call->uniformOffset == -1) {
            goto error;
        }

        NVGMTLFragmentUniforms *uniforms = [self fragmentUniformPointerAtOffset:call->uniformOffset];
        memset(uniforms, 0, sizeof(*uniforms));
        uniforms->strokeThr = -1.0f;
        uniforms->type = NVGMTL_SHADER_SIMPLE;
        
        [self _convertPaint:paint
                   scissor:scissor
                     width:fringe
                    fringe:fringe
           strokeThreshold:-1.0f
                  uniforms:[self fragmentUniformPointerAtOffset:call->uniformOffset + NVGMTLFragmentUniformStride]];
    } else {
        call->uniformOffset = [self _allocFragUniforms:1];
        if (call->uniformOffset == -1) {
            goto error;
        }

        [self _convertPaint:paint
                   scissor:scissor
                     width:fringe
                    fringe:fringe
           strokeThreshold:-1.0f
                  uniforms:[self fragmentUniformPointerAtOffset:call->uniformOffset]];
    }
    
    return;
    
error:
    if (_ncalls > 0) {
        --_ncalls;
    }
}

- (void)renderStroke:(NVGpaint *)paint
  compositeOperation:(NVGcompositeOperationState)compositeOperation
             scissor:(NVGscissor *)scissor
              fringe:(float)fringe
         strokeWidth:(float)strokeWidth
               paths:(const NVGpath *)paths
              npaths:(int)npaths
{
    NVGMTLCall *call = [self _allocCall];
    if (call == NULL) {
        return;
    }

    call->type = NVGMTL_STROKE;
    call->pathOffset = [self _allocPaths:npaths];
    if (call->pathOffset == -1) {
        goto error;
    }
    
    call->pathCount = npaths;
    call->image = paint->image;
    call->blendState = NVGMetalBlendStateFromCompOpState(compositeOperation);

    int maxverts = [self _maxVertCountForPaths:paths pathCount:npaths];
    int offset = [self _allocVerts:maxverts];
    if (offset == -1) {
        goto error;
    }
    
    for (int i = 0; i < npaths; i++) {
        NVGMTLPath *copy = &_paths[call->pathOffset + i];
        const NVGpath* path = &paths[i];
        memset(copy, 0, sizeof(NVGMTLPath));
        if (path->nstroke) {
            copy->strokeOffset = offset;
            copy->strokeCount = path->nstroke;
            memcpy(&_verts[offset], path->stroke, sizeof(NVGvertex) * path->nstroke);
            offset += path->nstroke;
        }
    }
    
    if (_flags & NVG_STENCIL_STROKES) {
        call->uniformOffset = [self _allocFragUniforms:2];
        if (call->uniformOffset == -1) {
            goto error;
        }
        
        [self _convertPaint:paint
                    scissor:scissor
                      width:strokeWidth
                     fringe:fringe
            strokeThreshold:-1.0f
                   uniforms:[self fragmentUniformPointerAtOffset:call->uniformOffset]];
        
        [self _convertPaint:paint
                    scissor:scissor
                      width:strokeWidth
                     fringe:fringe
            strokeThreshold:1.0f - 0.5f / 255.0f
                   uniforms:[self fragmentUniformPointerAtOffset:call->uniformOffset + NVGMTLFragmentUniformStride]];
        
    } else {
        call->uniformOffset =[self _allocFragUniforms:1];
        if (call->uniformOffset == -1) {
            goto error;
        }
        
        [self _convertPaint:paint
                    scissor:scissor
                      width:strokeWidth
                     fringe:fringe
            strokeThreshold:-1.0f
                   uniforms:[self fragmentUniformPointerAtOffset:call->uniformOffset]];
    }
    
    return;
    
error:
    if (_ncalls > 0) {
        --_ncalls;
    };
}

- (void)renderTriangles:(NVGpaint *)paint
     compositeOperation:(NVGcompositeOperationState)compositeOperation
                scissor:(NVGscissor *)scissor
                  verts:(const NVGvertex *)verts
                 nverts:(int)nverts
                 fringe:(float)fringe
{
    NVGMTLCall *call = [self _allocCall];
    if (call == NULL) {
        return;
    }

    call->type = NVGMTL_TRIANGLES;
    call->image = paint->image;
    call->blendState = NVGMetalBlendStateFromCompOpState(compositeOperation);

    call->baseVertex = [self _allocVerts:nverts];
    if (call->baseVertex == -1) {
        goto error;
    }
    call->triangleCount = nverts;
    
    memcpy(&_verts[call->baseVertex], verts, sizeof(NVGvertex) * nverts);

    call->uniformOffset = [self _allocFragUniforms:1];
    if (call->uniformOffset == -1) {
        goto error;
    }
    NVGMTLFragmentUniforms *uniforms = [self fragmentUniformPointerAtOffset:call->uniformOffset];
    [self _convertPaint:paint scissor:scissor width:1.0f fringe:fringe strokeThreshold:-1.0f uniforms:uniforms];
    uniforms->type = NVGMTL_SHADER_IMG;
    
    return;
    
error:
    if (_ncalls > 0) {
        --_ncalls;
    }
}

#pragma mark - Backend Internals (State)

- (void)_setRenderPipelineState:(id<MTLRenderPipelineState>)renderPipelineState {
    if (self.currentRenderPipelineState != renderPipelineState) {
        [self.currentRenderCommandEncoder setRenderPipelineState:renderPipelineState];
        self.currentRenderPipelineState = renderPipelineState;
    }
}

- (void)_setDepthStencilState:(id<MTLDepthStencilState>)depthStencilState {
    if (self.currentDepthStencilState != depthStencilState) {
        [self.currentRenderCommandEncoder setDepthStencilState:depthStencilState];
        self.currentDepthStencilState = depthStencilState;
    }
}

- (void)_setFragmentTexture:(id<MTLTexture> _Nullable)texture {
    if (self.currentTexture != texture) {
        [self.currentRenderCommandEncoder setFragmentTexture:texture atIndex:0];
        self.currentTexture = texture;
    }
}

#pragma mark - Backend Internals (Rendering)

- (BOOL)_convertPaint:(NVGpaint *)paint
              scissor:(NVGscissor *)scissor
                width:(float)width
               fringe:(float)fringe
      strokeThreshold:(float)strokeThr
             uniforms:(NVGMTLFragmentUniforms *)outUniforms
{
    NVGMTLTexture *tex = nil;
    float inverseTransform[6];
    
    NVGMTLFragmentUniforms uniforms;
    memset(&uniforms, 0, sizeof(uniforms));
    
    uniforms.innerCol = NVGMTLPremultiplyColor(paint->innerColor);
    uniforms.outerCol = NVGMTLPremultiplyColor(paint->outerColor);
    
    if (scissor->extent[0] < -0.5f || scissor->extent[1] < -0.5f) {
        memset(&uniforms.scissorMat, 0, sizeof(uniforms.scissorMat));
        uniforms.scissorExt[0] = 1.0f;
        uniforms.scissorExt[1] = 1.0f;
        uniforms.scissorScale[0] = 1.0f;
        uniforms.scissorScale[1] = 1.0f;
    } else {
        nvgTransformInverse(inverseTransform, scissor->xform);
        NVGMTLFloat3x4FromElements(uniforms.scissorMat, inverseTransform);
        uniforms.scissorExt[0] = scissor->extent[0];
        uniforms.scissorExt[1] = scissor->extent[1];
        uniforms.scissorScale[0] = sqrtf(scissor->xform[0] * scissor->xform[0] + scissor->xform[2] * scissor->xform[2]) / fringe;
        uniforms.scissorScale[1] = sqrtf(scissor->xform[1] * scissor->xform[1] + scissor->xform[3] * scissor->xform[3]) / fringe;
    }
    
    memcpy(&uniforms.extent, paint->extent, sizeof(uniforms.extent));
    uniforms.strokeMult = (width * 0.5f + fringe * 0.5f) / fringe;
    uniforms.strokeThr = strokeThr;
    
    if (paint->image != 0) {
        if ((tex = [self textureForIdentifier:paint->image]) == nil) {
            return NO;
        }
        
        if ((tex.flags & NVG_IMAGE_FLIPY) != 0) {
            float m1[6], m2[6];
            nvgTransformTranslate(m1, 0.0f, uniforms.extent[1] * 0.5f);
            nvgTransformMultiply(m1, paint->xform);
            nvgTransformScale(m2, 1.0f, -1.0f);
            nvgTransformMultiply(m2, m1);
            nvgTransformTranslate(m1, 0.0f, -uniforms.extent[1] * 0.5f);
            nvgTransformMultiply(m1, m2);
            nvgTransformInverse(inverseTransform, m1);
        } else {
            nvgTransformInverse(inverseTransform, paint->xform);
        }
        uniforms.type = NVGMTL_SHADER_FILLIMG;
        
        if (tex.type == NVG_TEXTURE_RGBA) {
            uniforms.texType = (tex.flags & NVG_IMAGE_PREMULTIPLIED) ? 0 : 1;
        } else {
            uniforms.texType = 2;
        }
    } else {
        uniforms.type = NVGMTL_SHADER_FILLGRAD;
        uniforms.radius = paint->radius;
        uniforms.feather = paint->feather;
        nvgTransformInverse(inverseTransform, paint->xform);
    }
    
    NVGMTLFloat3x4FromElements(uniforms.paintMat, inverseTransform);

    *outUniforms = uniforms;
    return YES;
}

- (NVGMTLFragmentUniforms *)fragmentUniformPointerAtOffset:(int)i {
    return (NVGMTLFragmentUniforms *)(_uniforms + i);
}

- (int)_maxVertCountForPaths:(const NVGpath *)paths pathCount:(int)pathCount {
    int count = 0;
    for (int i = 0; i < pathCount; ++i) {
        count += paths[i].nfill * 3; // TODO: Make this a tighter bound
        count += paths[i].nstroke;
    }
    return count;
}

- (void)_drawFill:(NVGMTLCall *)call {
    NVGMTLPath *paths = &_paths[call->pathOffset];
    int npaths = call->pathCount;
    
    id<MTLRenderCommandEncoder> renderCommandEncoder = self.currentRenderCommandEncoder;

    [self _setDepthStencilState:self.fillStencilState];
    [self _setRenderPipelineState:self.stencilPopulateRenderPipelineState];

    [self setUniformsForOffset:call->uniformOffset textureIdentifier:0];

    [renderCommandEncoder setCullMode:MTLCullModeNone];
    for (int i = 0; i < npaths; i++) {
        //glDrawArrays(GL_TRIANGLE_FAN, paths[i].fillOffset, paths[i].fillCount);
        [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                 vertexStart:paths[i].fillOffset
                                 vertexCount:paths[i].fillCount];
    }
    [renderCommandEncoder setCullMode:MTLCullModeBack];

    [self _setRenderPipelineState:self.fillRenderPipelineState];
    
    [self setUniformsForOffset:call->uniformOffset + NVGMTLFragmentUniformStride
             textureIdentifier:call->image];

    if (self.flags & NVG_ANTIALIAS) {
        [self _setDepthStencilState:self.equalStencilState];

        for (int i = 0; i < npaths; ++i) {
            [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                     vertexStart:paths[i].strokeOffset
                                     vertexCount:paths[i].strokeCount];
        }
    }
    [self _setDepthStencilState:self.zeroOnNotEqualStencilState];

    [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                             vertexStart:call->baseVertex
                             vertexCount:call->triangleCount];
}

- (void)_drawConvexFill:(NVGMTLCall *)call {
    NVGMTLPath *paths = &_paths[call->pathOffset];
    int npaths = call->pathCount;
    
    [self _setRenderPipelineState:self.fillRenderPipelineState];
    [self _setDepthStencilState:self.defaultStencilState];

    [self setUniformsForOffset:call->uniformOffset textureIdentifier:call->image];
    
    for (int i = 0; i < npaths; ++i) {
        //glDrawArrays(GL_TRIANGLE_FAN, paths[i].fillOffset, paths[i].fillCount);
        [self.currentRenderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                             vertexStart:paths[i].fillOffset
                                             vertexCount:paths[i].fillCount];
        if (paths[i].strokeCount > 0) {
            [self.currentRenderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                                 vertexStart:paths[i].strokeOffset
                                                 vertexCount:paths[i].strokeCount];
        }
    }
}

- (void)_drawStroke:(NVGMTLCall *)call {
    NVGMTLPath *paths = &_paths[call->pathOffset];
    int npaths = call->pathCount;
    
    id<MTLRenderCommandEncoder> renderCommandEncoder = self.currentRenderCommandEncoder;
    
    if (_flags & NVG_STENCIL_STROKES) {
        [self _setDepthStencilState:self.incrementOnEqualStencilState];

        [self setUniformsForOffset:call->uniformOffset + NVGMTLFragmentUniformStride
                 textureIdentifier:call->image];
        
        for (int i = 0; i < npaths; ++i) {
            [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                     vertexStart:paths[i].strokeOffset
                                     vertexCount:paths[i].strokeCount];
        }

        [self setUniformsForOffset:call->uniformOffset textureIdentifier:call->image];

        [self _setDepthStencilState:self.equalStencilState];

        for (int i = 0; i < npaths; ++i) {
            [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                     vertexStart:paths[i].strokeOffset
                                     vertexCount:paths[i].strokeCount];
        }
        
        [self _setDepthStencilState:self.alwaysZeroStencilState];

        for (int i = 0; i < npaths; ++i) {
            [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                     vertexStart:paths[i].strokeOffset
                                     vertexCount:paths[i].strokeCount];
        }

        [self _setRenderPipelineState:self.fillRenderPipelineState];
        [self _setDepthStencilState:self.defaultStencilState];
    } else {
        [self setUniformsForOffset:call->uniformOffset textureIdentifier:call->image];

        for (int i = 0; i < npaths; ++i) {
            [renderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                     vertexStart:paths[i].strokeOffset
                                     vertexCount:paths[i].strokeCount];
        }
    }
}

- (void)_drawTriangles:(NVGMTLCall *)call {
    [self _setRenderPipelineState:self.fillRenderPipelineState];

    [self _setDepthStencilState:self.defaultStencilState];

    [self setUniformsForOffset:call->uniformOffset textureIdentifier:call->image];

    [self.currentRenderCommandEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                         vertexStart:call->baseVertex
                                         vertexCount:call->triangleCount];
}

#pragma mark - Backend Internals (Memory Management)

- (NVGMTLCall *)_allocCall {
    if (_ncalls + 1 > _ccalls) {
        NVGMTLCall *calls;
        int ccalls = MAX(_ncalls + 1, 128) + _ccalls / 2;
        calls = (NVGMTLCall *)realloc(_calls, sizeof(NVGMTLCall) * ccalls);
        if (calls == NULL) {
            return NULL;
        }
        _calls = calls;
        _ccalls = ccalls;
    }
    NVGMTLCall *ret = &_calls[_ncalls++];
    memset(ret, 0, sizeof(NVGMTLCall));
    return ret;
}

- (int)_allocPaths:(int)n {
    if (_npaths + n > _cpaths) {
        NVGMTLPath *paths;
        int cpaths = MAX(_npaths + n, 128) + _cpaths / 2;
        paths = (NVGMTLPath *)realloc(_paths, sizeof(NVGMTLPath) * cpaths);
        if (paths == NULL) {
          return -1;
        }
        _paths = paths;
        _cpaths = cpaths;
    }
    int ret = _npaths;
    _npaths += n;
    return ret;
}

- (int)_allocVerts:(int)n {
    if (_nverts+n > _cverts) {
        NVGvertex *verts;
        int cverts = MAX(_nverts + n, 4096) + _cverts / 2;
        verts = (NVGvertex *)realloc(_verts, sizeof(NVGvertex) * cverts);
        if (verts == NULL) {
            return -1;
        }
        _verts = verts;
        _cverts = cverts;
    }
    int ret = _nverts;
    _nverts += n;
    return ret;
}

- (int)_allocFragUniforms:(int)n {
    int structSize = NVGMTLFragmentUniformStride;
    if (_nuniforms+n > _cuniforms) {
        unsigned char *uniforms;
        int cuniforms = MAX(_nuniforms + n, 128) + _cuniforms / 2;
        uniforms = (unsigned char *)realloc(_uniforms, structSize * cuniforms);
        if (uniforms == NULL) {
            return -1;
        }
        _uniforms = uniforms;
        _cuniforms = cuniforms;
    }
    int ret = _nuniforms * structSize;
    _nuniforms += n;
    return ret;
}

- (void)_enqueueReusableBuffer:(id<MTLBuffer>)buffer {
    @synchronized (self) {
        [_reusableBufferPool addObject:buffer];
    }
}

- (id<MTLBuffer>)_dequeueReusableBufferOfLength:(size_t)length {
    id<MTLBuffer> buffer = nil;
    @synchronized (self) {
        int reuseIndex = -1;
        for (int i = 0; i < _reusableBufferPool.count; ++i) {
            id<MTLBuffer> candidate = _reusableBufferPool[i];
            if (candidate.length >= length) {
                reuseIndex = i;
                break;
            }
        }
        
        if (reuseIndex >= 0) {
            buffer = _reusableBufferPool[reuseIndex];
            [_reusableBufferPool removeObjectAtIndex:reuseIndex];
        } else {
            buffer = [_device newBufferWithLength:length options:MTLResourceStorageModeShared];
            NSLog(@"Allocated buffer of length %d", (int)length);
        }
    }
    return buffer;
}

@end

#pragma mark - Internal Bridging Interface

static int NVGMTLRenderCreate(void *usr) {
    return 1;
}

static int NVGMTLRenderCreateTexture(void *usr, int type, int w, int h, int imageFlags,
                                     const unsigned char* _Nullable data)
{
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    return [mtlContext makeTextureWithBytes:data type:type width:w height:h flags:imageFlags];
}

static int NVGMTLRenderDeleteTexture(void *usr, int image) {
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    return [mtlContext removeTextureForIdentifier:image];
}

static int NVGMTLRenderUpdateTexture(void *usr, int image, int originX, int originY, int width, int height,
                                     const unsigned char* data)
{
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    return [mtlContext replaceRegion:MTLRegionMake2D(originX, originY, width, height)
                           ofTexture:image
                           withBytes:data];
}

static int NVGMTLRenderGetTextureSize(void *usr, int image, int* width, int* height) {
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    return [mtlContext getSizeOfTextureForIdentifier:image width:width height:height];
}

static void NVGMTLRenderViewport(void *usr, float width, float height, float devicePixelRatio) {
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    [mtlContext setViewportWidth:width height:height];
}

static void NVGMTLRenderCancel(void *usr) {
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    [mtlContext renderCancel];
}

static void NVGMTLRenderFlush(void *usr) {
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    [mtlContext renderFlush];
}

static void NVGMTLRenderFill(void *usr, NVGpaint* paint, NVGcompositeOperationState compositeOperation,
                             NVGscissor* scissor, float fringe, const float* bounds, const NVGpath* paths, int npaths)
{
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    [mtlContext renderFill:paint
        compositeOperation:compositeOperation
                   scissor:scissor
                    fringe:fringe
                    bounds:bounds
                     paths:paths
                    npaths:npaths];
}

static void NVGMTLRenderStroke(void *usr, NVGpaint* paint, NVGcompositeOperationState compositeOperation,
                               NVGscissor* scissor, float fringe, float strokeWidth, const NVGpath* paths, int npaths)
{
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    [mtlContext renderStroke:paint
          compositeOperation:compositeOperation
                     scissor:scissor
                      fringe:fringe
                 strokeWidth:strokeWidth
                       paths:paths
                      npaths:npaths];
}

static void NVGMTLRenderTriangles(void *usr, NVGpaint* paint, NVGcompositeOperationState compositeOperation,
                                  NVGscissor* scissor, const NVGvertex* verts, int nverts, float fringe)
{
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    [mtlContext renderTriangles:paint
             compositeOperation:compositeOperation
                        scissor:scissor
                          verts:verts
                         nverts:nverts
                         fringe:fringe];
}

static void NVGMTLRenderDelete(void *usr) {
}

#pragma mark - Public API

NVGcontext *nvgCreateMetal(id<MTLDevice> device, id<MTLCommandQueue> commandQueue, int flags)
{
    NVGMTLContext *gl = [[NVGMTLContext alloc] initWithDevice:device commandQueue:commandQueue flags:flags];
    
    NSUInteger contextSlot = NVGMTLContext.nextContextSlot;
    NVGMTLContext.contextRegistry[@(contextSlot)] = gl;
    NVGMTLContext.nextContextSlot = contextSlot + 1;

    NVGparams params;
    memset(&params, 0, sizeof(params));
    params.renderCreate = NVGMTLRenderCreate;
    params.renderCreateTexture = NVGMTLRenderCreateTexture;
    params.renderDeleteTexture = NVGMTLRenderDeleteTexture;
    params.renderUpdateTexture = NVGMTLRenderUpdateTexture;
    params.renderGetTextureSize = NVGMTLRenderGetTextureSize;
    params.renderViewport = NVGMTLRenderViewport;
    params.renderCancel = NVGMTLRenderCancel;
    params.renderFlush = NVGMTLRenderFlush;
    params.renderFill = NVGMTLRenderFill;
    params.renderStroke = NVGMTLRenderStroke;
    params.renderTriangles = NVGMTLRenderTriangles;
    params.renderDelete = NVGMTLRenderDelete;
    params.userPtr = (void *)contextSlot;
    params.edgeAntiAlias = flags & NVG_ANTIALIAS ? 1 : 0;

    NVGcontext *ctx = nvgCreateInternal(&params);
    if (ctx == NULL) {
        goto error;
    }
    
    return ctx;
    
error:
    if (ctx != NULL) {
        nvgDeleteInternal(ctx);
    }
    return NULL;
}

void nvgDeleteMetal(NVGcontext *ctx) {
}

void nvgBeginFrameMetal(NVGcontext *ctx, id<MTLCommandBuffer> commandBuffer, id<MTLRenderCommandEncoder> rce) {
    void *usr = nvgInternalParams(ctx)->userPtr;
    NVGMTLContext *mtlContext = NVGMTLContextRegistry[@((int)usr)];
    mtlContext.currentCommandBuffer = commandBuffer;
    mtlContext.currentRenderCommandEncoder = rce;
}

NS_ASSUME_NONNULL_END

#endif /* NANOVG_METAL_IMPLEMENTATION */
