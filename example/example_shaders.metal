#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float2 tcoord [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 ftcoord;
    float2 fpos;
};

struct VertexUniforms {
    float2 viewSize;
};

struct FragmentUniforms {
    float3x3 scissorMat;
    float3x3 paintMat;
    float4 innerCol;
    float4 outerCol;
    float2 scissorExt;
    float2 scissorScale;
    float2 extent;
    float radius;
    float feather;
    float strokeMult;
    float strokeThr;
    int texType;
    int type;
};

vertex VertexOut vertex_main(VertexIn in [[stage_in]],
                             constant VertexUniforms& uniforms [[buffer(1)]])
{
    VertexOut out {
        .ftcoord = in.tcoord,
        .fpos = in.position,
        .position = float4(2.0f * in.position.x / uniforms.viewSize.x - 1.0f,
                           1.0f - 2.0f * in.position.y / uniforms.viewSize.y,
                           0.0f,
                           1.0f)
    };
    return out;
}

static float sdroundrect(float2 pt, float2 ext, float rad) {
    float2 ext2 = ext - float2(rad, rad);
    float2 d = abs(pt) - ext2;
    return min(max(d.x, d.y), 0.0f) + length(max(d, 0.0f)) - rad;
}

static float scissorMask(float2 p, float3x3 scissorMat, float2 scissorScale, float2 scissorExt) {
    float2 sc = (abs((scissorMat * float3(p, 1.0f)).xy) - scissorExt);
    sc = float2(0.5f) - sc * scissorScale;
    return saturate(sc.x) * saturate(sc.y);
}

static float strokeMask(float2 ftcoord, float strokeMult) {
    return min(1.0f, (1.0f - abs(ftcoord.x * 2.0f - 1.0f)) * strokeMult) * min(1.0f, ftcoord.y);
};

constexpr sampler linearSampler { coord::normalized, address::repeat, filter::linear, mip_filter::linear };

fragment half4 fragment_main(VertexOut in [[stage_in]],
                             constant FragmentUniforms& uniforms [[buffer(0)]],
                             texture2d<half, access::sample> tex [[texture(0)]])
{
    half4 result {0.0h};

    half scissor = scissorMask(in.fpos, uniforms.scissorMat, uniforms.scissorScale, uniforms.scissorExt);
    half strokeAlpha = strokeMask(in.ftcoord, uniforms.strokeMult);
    if (strokeAlpha < uniforms.strokeThr) {
        discard_fragment();
    }
    switch (uniforms.type) {
        case 0: {
            float2 pt = (uniforms.paintMat * float3(in.fpos, 1.0f)).xy;
            float d = saturate((sdroundrect(pt, uniforms.extent, uniforms.radius) + uniforms.feather * 0.5f) / uniforms.feather);
            half4 color = (half4)mix(uniforms.innerCol, uniforms.outerCol, d);
            color *= strokeAlpha * scissor;
            result = color;
            break;
        }
        case 1: {
            float2 pt = (uniforms.paintMat * float3(in.fpos, 1.0f)).xy / uniforms.extent;
            half4 color = tex.sample(linearSampler, pt);
            if (uniforms.texType == 1) {
                color = half4(color.xyz * color.w, color.w);
            }
            if (uniforms.texType == 2) {
                color = half4(color.x);
            }
            color *= (half4)(uniforms.innerCol);
            color *= (half)(strokeAlpha * scissor);
            result = color;
            break;
        }
        case 2:
            result = half4(1.0h);
            break;
        case 3: {
            half4 color = tex.sample(linearSampler, in.ftcoord);
            if (uniforms.texType == 1) {
                color = half4(color.rgb * color.a, color.a);
            } else if (uniforms.texType == 2) {
                color = half4(color.r);
            }
            color *= scissor;
            result = color * (half4)uniforms.innerCol;
            break;
        }
        default:
            break;
    }
    return result;
}
