#ifdef MNN_USE_SSE
#include "../x86_x64/sse/FunctionSummary.hpp"
#include "../x86_x64/avx/FunctionSummary.hpp"
#include "../x86_x64/avxfma/FunctionSummary.hpp"
#include "../x86_x64/avx512/FunctionSummary.hpp"
#include "../x86_x64/cpu_id.h"
#endif
#include "core/Macro.h"
#if defined(MNN_USE_NEON)
#include "../arm/FunctionSummary.hpp"
#endif

#include "BF16Functions.hpp"
#include "WinogradOptFunctionHalf.hpp"
#include "../compute/CommonOptFunction.h"
#include "VecHalf.hpp"
#include "math/Vec.hpp"
using BFVec4 = MNN::Math::VecHalf<4>;
using Vec4 = MNN::Math::Vec<float, 4>;
namespace MNN {
// just for reference BF16 converting of c++ code, not for arm or sse.
inline int16_t MNNFP32ToBF16(float fp32Value) {
    int32_t* s32Value = (int32_t*)(&fp32Value);
    return (int16_t)((*s32Value) >> 16);
}
inline float MNNLowpToFp32(int16_t s16Value) {
    int32_t s32Value = ((int32_t)s16Value) << 16;
    float* fp32Value = (float*)(&s32Value);
    return *fp32Value;
}

static void _MNNFp32ToLowp(const float* src, int16_t* dst, size_t size) {
    int sizeC4 = size / 4;
    for (int i = 0; i < sizeC4; ++i) {
        auto srcV = Vec4::load(src);
        auto dstV = BFVec4(std::move(srcV.value));
        BFVec4::save(dst, dstV);
        src+=4;
        dst+=4;
    }
    int sizeRemain = size % 4;
    if (sizeRemain > 0) {
        float srcTemp[4];
        int64_t dstTemp[1];
        ::memcpy(srcTemp, src, sizeRemain * sizeof(float));
        auto srcV = Vec4::load(srcTemp);
        auto dstV = BFVec4(std::move(srcV.value));
        BFVec4::save((int16_t*)dstTemp, dstV);
        ::memcpy(dst, dstTemp, sizeRemain * sizeof(int16_t));
    }
}
static void _MNNLowpToFp32(const int16_t* src, float* dst, size_t size) {
    int sizeC4 = size / 4;
    for (int i = 0; i < sizeC4; ++i) {
        auto srcV = BFVec4::load(src);
        auto dstV = Vec4(std::move(srcV.value));
        Vec4::save(dst, dstV);
        src+=4;
        dst+=4;
    }
    int sizeRemain = size % 4;
    if (sizeRemain > 0) {
        int64_t srcTemp[2];
        float dstTemp[4];
        ::memcpy(srcTemp, src, sizeRemain * sizeof(int16_t));
        auto srcV = BFVec4::load((int16_t*)srcTemp);
        auto dstV = Vec4(std::move(srcV.value));
        Vec4::save(dstTemp, dstV);
        ::memcpy(dst, dstTemp, sizeRemain * sizeof(float));
    }
}
static void MNNConvRunForUnitDepthWiseBF16(float* dst, const float* src, const float* weight, size_t fw, size_t fh,
                                size_t weight_y_step, size_t dilateX_step, size_t dilateY_step) {
    int fx, fy;
    BFVec4 dstValue(0.0f);
    const int16_t* src_z    = (const int16_t*)src;
    const int16_t* weight_z = (const int16_t*)weight;
    for (fy = 0; fy < fh; ++fy) {
        const auto src_y    = src_z + fy * dilateY_step;
        const auto weight_y = weight_z + fy * weight_y_step;
        for (fx = 0; fx < fw; ++fx) {
            const auto weight_x = weight_y + 4 * fx;
            const auto src_x    = src_y + fx * dilateX_step;
            dstValue = dstValue + BFVec4::load(src_x) * BFVec4::load(weight_x);
        }
    }
    BFVec4::save((int16_t*)dst, dstValue);
}

static void MNNConvRunForLineDepthwiseBF16(float* dstO, const float* srcO, const float* weightO, size_t width, size_t src_w_setup,
                                size_t fw, size_t fh, size_t dilateX_step, size_t dilateY_step, size_t height,
                                    size_t srcHStep, size_t dstHStep) {
    int dx, fx, fy;
    auto dst = (int16_t*)dstO;
    auto src = (const int16_t*)srcO;
    auto weight = (const int16_t*)weightO;
    for (int y = 0; y < height; ++y) {
        auto srcY = src + y * srcHStep;
        auto dstY = dst + y * dstHStep;
        for (dx = 0; dx < width; ++dx) {
            auto dst_x          = dstY + dx * 4;
            BFVec4 dstValue(0.0f);
            const auto src_z    = srcY + src_w_setup * dx;
            const auto weight_z = weight;
            for (fy = 0; fy < fh; ++fy) {
                const auto src_y    = src_z + fy * dilateY_step;
                const auto weight_y = weight_z + fy * fw * 4;
                for (fx = 0; fx < fw; ++fx) {
                    const auto weight_x = weight_y + 4 * fx;
                    const auto src_x    = src_y + fx * dilateX_step;
                    dstValue = dstValue + BFVec4::load(src_x) * BFVec4::load(weight_x);
                }
            }
            BFVec4::save(dst_x, dstValue);
        }
    }
}
void MNNAxByClampBroadcastUnitBF16(float* CF, const float* AF, const float* BF, size_t width, size_t cStride, size_t aStride, size_t height, const float* parameters) {
    auto C = (int16_t*)CF;
    auto A = (const int16_t*)AF;
    auto B = (const int16_t*)BF;
    auto minF = BFVec4(parameters[2]);
    auto maxF = BFVec4(parameters[3]);
    auto beta = BFVec4(parameters[1]);
    for (int y = 0; y < height; ++y) {
        auto a = A + aStride * y;
        auto b = B + 4 * y;
        auto bv = BFVec4::load(b);
        auto c = C + cStride * y;
        for (int x = 0; x < width; ++x) {
            auto av = BFVec4::load(a + 4 * x);
            auto cv = av + bv * beta;
            cv = BFVec4::min(cv, maxF);
            cv = BFVec4::max(cv, minF);
            BFVec4::save(c + 4 * x, cv);
        }
    }
}

#if !defined(MNN_USE_SSE) && !defined(MNN_USE_NEON)
void MNNPackC4ForMatMul_A_BF16(float* destOrigin, float const** sourceGroup, const int32_t* info, const int32_t* el) {
    MNNPackC4ForMatMul_A(destOrigin, sourceGroup, info, el);
    return;
}

void MNNPackForMatMul_B_BF16(float* dest, const float* source, size_t h, size_t l, bool transpose) {
    auto hP = h / 4;
    auto hR = hP * 4;
    if (hR != h) {
        ::memset(dest, 0, UP_DIV(h, 4)*4*l*sizeof(int16_t));
    }
    if (!transpose) {
        for (int y=0; y<hP; ++y) {
            auto destY = dest + y * 4 * l;
            auto sourceY = source + y * 4;
            for (int x=0; x<l; ++x) {
                ::memcpy(destY + 4 * x, sourceY + x * h, 4 * sizeof(int16_t));
            }
        }
        auto hRemain = h - hR;
        if (hRemain > 0) {
            auto destY = dest + hP * 4 * l;
            auto sourceY = source + hP * 4;
            for (int x=0; x<l; ++x) {
                ::memcpy(destY + 4 * x, sourceY + x * h, hRemain * sizeof(int16_t));
            }
        }
        return;
    }
    MNNPackC4Int16((int16_t*)dest, (const int16_t*)source, l, h);
}
#endif

void MNNPackedMatMulRemain_BF16(float* CFloat, const float* AFloat, const float* BFloat, size_t eSize,
                                const size_t* parameter, float* cacheFloat, const float* postParameters,
                                const float* biasFloat) {
    int16_t* C        = (int16_t*)CFloat;
    int16_t* A        = (int16_t*)AFloat;
    int16_t* B        = (int16_t*)BFloat;
    int16_t* cache    = (int16_t*)cacheFloat;
    int16_t* bias     = (int16_t*)biasFloat;
    auto h            = parameter[2];
    auto l            = parameter[1];
    auto cStride      = parameter[3] / sizeof(int16_t);
    auto hRemain      = parameter[4];
    auto bExtraStride = parameter[5] / sizeof(int16_t);
    auto bStride      = bExtraStride + l * 6;
    auto hC4          = UP_DIV(h, 4);
    for (int y = 0; y < hC4; ++y) {
        ::memset(C + y * cStride, 0, eSize * 4 * sizeof(int16_t));
    }
    float alpha    = 1.0f;
    float beta     = 0.0f;
    float minValue = -std::numeric_limits<float>().max();
    float maxValue = std::numeric_limits<float>().max();
    if (nullptr != postParameters) {
        minValue = postParameters[2];
        maxValue = postParameters[3];
        alpha    = postParameters[0];
        beta     = postParameters[1];
    }

    for (int x = 0; x < eSize; ++x) {
        auto dst = C + 4 * x;
        auto src =
            A + x; // input data is packed as tileCount x l x 16, is only one tiled block here, indexed as A[z * 16 + x]
        for (int ry = 0; ry < h; ++ry) {
            auto y        = ry / 4;
            auto yRemain  = ry % 4;
            auto bY       = B + y * bStride;
            auto dstY     = dst + y * cStride; // convert NCHW to NC4HW4 ie 1·(y/4)·X·4
            int wdy       = ry / 6;
            int wdyRemain = ry % 6;
            auto weight =
                B + wdy * bStride +
                wdyRemain; // weight is packed as (h/6) x l x 6, indexed as B[(ry / 6) * Bstride +z*6 + (ry % 6)]
            float summer = 0.0f;
            for (int z = 0; z < l; ++z) {
                auto aZ = src + z * 16;
                auto wZ = weight + z * 6;
                summer += MNNLowpToFp32(wZ[0]) * MNNLowpToFp32(aZ[0]);
            }
            float originValue = MNNLowpToFp32(dstY[yRemain]);
            if (nullptr != bias) {
                originValue = MNNLowpToFp32(bias[ry]);
            }
            auto dstValue = originValue * beta + alpha * summer;
            dstValue      = std::min(dstValue, maxValue);
            dstValue      = std::max(dstValue, minValue);
            dstY[yRemain] = MNNFP32ToBF16(dstValue);
        }
    }
}

void MNNPackedMatMul_BF16(float* C, const float* A, const float* B, const size_t* parameter, float* cache,
                          const float* postParameters, const float* bias) {
    return MNNPackedMatMulRemain_BF16(C, A, B, 16, parameter, cache, postParameters, bias);
    // return _AVX_MNNPackedMatMulFMA(C, A, B, parameter, cache);
}


static void _MNNConvDwF23MulTransUnit(float **cacheLine, const float *weigth, float *dest, size_t ow);

static void _MNNMultiAndDestTransformCommon23(float **cacheLine, const float *weigthF, float *destF, int cacheLineSize, int ow) {
    auto weigth = (const int16_t*)weigthF;
    auto dest = (int16_t*)destF;
    int unit = ow / 2;
    MNN_ASSERT(cacheLineSize >= 1);
    for (int x = 0; x < unit; ++x) {
        auto offset = 4 * 4 * x;
        int i = 0;
        BFVec4 m0     = BFVec4::load(weigth + i * 16 + 4 * 0) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 0);
        BFVec4 m1     = BFVec4::load(weigth + i * 16 + 4 * 1) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 1);
        BFVec4 m2     = BFVec4::load(weigth + i * 16 + 4 * 2) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 2);
        BFVec4 m3     = BFVec4::load(weigth + i * 16 + 4 * 3) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 3);

        for (i = 1; i < cacheLineSize; ++i) {
            m0 = m0 + BFVec4::load(weigth + i * 16 + 4 * 0) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 0);
            m1 = m1 + BFVec4::load(weigth + i * 16 + 4 * 1) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 1);
            m2 = m2 + BFVec4::load(weigth + i * 16 + 4 * 2) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 2);
            m3 = m3 + BFVec4::load(weigth + i * 16 + 4 * 3) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 3);
        }

        auto o0 = m0 + m1 + m2;
        auto o1 = m1 - m2 + m3;
        BFVec4::save(dest + 8 * x + 0 * 4, o0);
        BFVec4::save(dest + 8 * x + 1 * 4, o1);
    }
    if (unit * 2 < ow) {
        auto offset = 4 * 4 * unit;
        int i = 0;
        BFVec4 m0     = BFVec4::load(weigth + i * 16 + 4 * 0) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 0);
        BFVec4 m1     = BFVec4::load(weigth + i * 16 + 4 * 1) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 1);
        BFVec4 m2     = BFVec4::load(weigth + i * 16 + 4 * 2) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 2);

        for (i = 1; i < cacheLineSize; ++i) {
            m0 = m0 + BFVec4::load(weigth + i * 16 + 4 * 0) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 0);
            m1 = m1 + BFVec4::load(weigth + i * 16 + 4 * 1) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 1);
            m2 = m2 + BFVec4::load(weigth + i * 16 + 4 * 2) * BFVec4::load((int16_t*)cacheLine[i] + offset + 4 * 2);
        }

        auto o0 = m0 + m1 + m2;
        BFVec4::save(dest + 8 * unit + 0 * 4, o0);
    }
}
static void _MNNConvDwF23SourceTransUnit(const int16_t *source, int16_t *dest, size_t unit);
static void _MNNSourceTransformCommonF23(const float *sourceF, float *destF, int unit, int iw, int pad, int su, int eu) {
    auto source = (const int16_t*)sourceF;
    auto dest = (int16_t*)destF;
    for (int x = 0; x < su; ++x) {
        auto dstX = dest + 4 * 4 * x;
        auto sx   = x * 2 - (int)pad;
        auto ex   = sx + 4;

        auto clampSx = std::max(sx, 0);
        auto clampEx = std::min(ex, (int)iw);

        BFVec4 v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = clampSx; i < clampEx; ++i) {
            v[i - sx] = BFVec4::load(source + 4 * i);
        }
        auto m0 = v[0] - v[2];
        auto m1 = v[1] + v[2];
        auto m2 = v[2] - v[1];
        auto m3 = v[3] - v[1];

        BFVec4::save(dstX + 4 * 0, m0);
        BFVec4::save(dstX + 4 * 1, m1);
        BFVec4::save(dstX + 4 * 2, m2);
        BFVec4::save(dstX + 4 * 3, m3);
    }
    _MNNConvDwF23SourceTransUnit(source + 4 * (su * 2 - pad), dest + 4 * 4 * su, eu - su);

    for (int x = eu; x < unit; ++x) {
        auto dstX = dest + 4 * 4 * x;
        auto sx   = x * 2 - (int)pad;
        auto ex   = sx + 4;

        auto clampSx = std::max(sx, 0);
        auto clampEx = std::min(ex, (int)iw);

        BFVec4 v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = clampSx; i < clampEx; ++i) {
            v[i - sx] = BFVec4::load(source + 4 * i);
        }
        auto m0 = v[0] - v[2];
        auto m1 = v[1] + v[2];
        auto m2 = v[2] - v[1];
        auto m3 = v[3] - v[1];

        BFVec4::save(dstX + 4 * 0, m0);
        BFVec4::save(dstX + 4 * 1, m1);
        BFVec4::save(dstX + 4 * 2, m2);
        BFVec4::save(dstX + 4 * 3, m3);
    }
}

static void _MNNConvDwF23MulTransUnit(float **cacheLine, const float *weigthF, float *destF, size_t ow) {
    int unit = ow / 2;
    auto weigth = (const int16_t*)weigthF;
    auto dest = (int16_t*)destF;

    auto w00 = BFVec4::load(weigth + 0 * 16 + 4 * 0);
    auto w01 = BFVec4::load(weigth + 0 * 16 + 4 * 1);
    auto w02 = BFVec4::load(weigth + 0 * 16 + 4 * 2);
    auto w03 = BFVec4::load(weigth + 0 * 16 + 4 * 3);
    auto w10 = BFVec4::load(weigth + 1 * 16 + 4 * 0);
    auto w11 = BFVec4::load(weigth + 1 * 16 + 4 * 1);
    auto w12 = BFVec4::load(weigth + 1 * 16 + 4 * 2);
    auto w13 = BFVec4::load(weigth + 1 * 16 + 4 * 3);
    auto w20 = BFVec4::load(weigth + 2 * 16 + 4 * 0);
    auto w21 = BFVec4::load(weigth + 2 * 16 + 4 * 1);
    auto w22 = BFVec4::load(weigth + 2 * 16 + 4 * 2);
    auto w23 = BFVec4::load(weigth + 2 * 16 + 4 * 3);
    for (int x = 0; x < unit; ++x) {
        auto offset = 4 * 4 * x;
        int i = 0;
        BFVec4 m0     = w00 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 0);
        BFVec4 m1     = w01 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 1);
        BFVec4 m2     = w02 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 2);
        BFVec4 m3     = w03 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 3);

        m0 = m0 + w10 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 0);
        m1 = m1 + w11 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 1);
        m2 = m2 + w12 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 2);
        m3 = m3 + w13 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 3);

        m0 = m0 + w20 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 0);
        m1 = m1 + w21 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 1);
        m2 = m2 + w22 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 2);
        m3 = m3 + w23 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 3);

        auto o0 = m0 + m1 + m2;
        auto o1 = m1 - m2 + m3;
        BFVec4::save(dest + 8 * x + 0 * 4, o0);
        BFVec4::save(dest + 8 * x + 1 * 4, o1);
    }
    if (unit * 2 < ow) {
        auto offset = 4 * 4 * unit;
        BFVec4 m0     = w00 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 0);
        BFVec4 m1     = w01 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 1);
        BFVec4 m2     = w02 * BFVec4::load((int16_t*)cacheLine[0] + offset + 4 * 2);

        m0 = m0 + w10 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 0);
        m1 = m1 + w11 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 1);
        m2 = m2 + w12 * BFVec4::load((int16_t*)cacheLine[1] + offset + 4 * 2);

        m0 = m0 + w20 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 0);
        m1 = m1 + w21 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 1);
        m2 = m2 + w22 * BFVec4::load((int16_t*)cacheLine[2] + offset + 4 * 2);
        auto o0 = m0 + m1 + m2;
        BFVec4::save(dest + 8 * unit + 0 * 4, o0);
    }
}
static void _MNNConvDwF23SourceTransUnit(const int16_t *source, int16_t *dest, size_t unit) {
    if (unit <= 0) {
        return;
    }
    BFVec4 v0 = BFVec4::load(source + 4 * 0);
    BFVec4 v1 = BFVec4::load(source + 4 * 1);
    BFVec4 v2;
    BFVec4 v3;
    source += 8;

    for (int x = 0; x < unit; ++x) {
        v2 = BFVec4::load(source + 0 * 4);
        v3 = BFVec4::load(source + 1 * 4);
        auto m0 = v0 - v2;
        auto m1 = v1 + v2;
        auto m2 = v2 - v1;
        auto m3 = v3 - v1;

        BFVec4::save(dest + 4 * 0, m0);
        BFVec4::save(dest + 4 * 1, m1);
        BFVec4::save(dest + 4 * 2, m2);
        BFVec4::save(dest + 4 * 3, m3);

        source += 8;
        dest += 16;

        v0 = v2;
        v1 = v3;
    }
}

static void _MNNMatrixSub(float* CF, const float* AF, const float* BF, size_t widthC4, size_t cStride, size_t aStride,
                  size_t bStride, size_t height) {
    auto A = (int16_t*)AF;
    auto B = (int16_t*)BF;
    auto C = (int16_t*)CF;
    for (int y = 0; y < height; ++y) {
        auto a = A + aStride * y;
        auto b = B + bStride * y;
        auto c = C + cStride * y;
        for (int x = 0; x < widthC4; ++x) {
            BFVec4::save(c + 4 * x, BFVec4::load(a + 4 * x) - BFVec4::load(b + 4 * x));
        }
    }
}
static void _MNNMatrixAdd(float* CF, const float* AF, const float* BF, size_t widthC4, size_t cStride, size_t aStride,
                  size_t bStride, size_t height) {
    auto A = (int16_t*)AF;
    auto B = (int16_t*)BF;
    auto C = (int16_t*)CF;
    for (int y = 0; y < height; ++y) {
        auto a = A + aStride * y;
        auto b = B + bStride * y;
        auto c = C + cStride * y;
        for (int x = 0; x < widthC4; ++x) {
            BFVec4::save(c + 4 * x, BFVec4::load(a + 4 * x) + BFVec4::load(b + 4 * x));
        }
    }
}

static void _MNNStrassenMergeCFunction(float* c11F, float* c12F, float* c21F, float* c22F, float* xAddrF, size_t cStride,
                               size_t eSub, size_t hSub) {
    auto c11 = (int16_t*)c11F;
    auto c12 = (int16_t*)c12F;
    auto c21 = (int16_t*)c21F;
    auto c22 = (int16_t*)c22F;
    auto xAddr = (int16_t*)xAddrF;
    for (int y=0; y<hSub; ++y) {
        auto c11Y = c11 + y * cStride;
        auto c12Y = c12 + y * cStride;
        auto c22Y = c22 + y * cStride;
        auto c21Y = c21 + y * cStride;
        auto xY = xAddr + y * eSub * 4;
        for (int x=0; x<eSub; ++x) {
            auto xv = BFVec4::load(xY + 4*x);
            auto c21v = BFVec4::load(c21Y + 4*x);
            auto c11v = BFVec4::load(c11Y + 4*x);
            auto c22v = BFVec4::load(c22Y + 4*x);
            auto c12v = BFVec4::load(c12Y + 4*x);
            c12v = c12v + xv;
            c21v = c12v + c21v;
            c12v = c22v + c12v;
            c22v = c22v + c21v;
            c12v = c11v + c12v;
            BFVec4::save(c12Y + 4*x, c12v);
            BFVec4::save(c22Y + 4*x, c22v);
            BFVec4::save(c21Y + 4*x, c21v);
        }
    }
}
static void _MNNScaleAndAddBias(float* dstF, const float* srcF, const float* biasF, const float* alphaF, size_t planeNumber,
                        size_t biasNumber) {
    auto dst = (int16_t*)dstF;
    auto src = (int16_t*)srcF;
    auto bias = (int16_t*)biasF;
    auto alpha = (int16_t*)alphaF;
    for (int z = 0; z < biasNumber; ++z) {
        auto dstZ         = dst + planeNumber * 4 * z;
        auto srcZ   = src + planeNumber * 4 * z;
        auto biasZ = BFVec4::load(bias + 4 * z);
        auto alphaZ = BFVec4::load(alpha + 4 * z);
        for (int p = 0; p < planeNumber; ++p) {
            auto dstX       = dstZ + 4 * p;
            auto srcX = srcZ + 4 * p;
            BFVec4::save(dstX, (BFVec4::load(srcX) * alphaZ) + biasZ);
        }
    }
}

static void _MNNAddC4WithStride(const float* sourceF, float* destF, size_t srcStride, size_t dstStride, size_t count) {
    auto source = (const int16_t*)sourceF;
    auto dest = (int16_t*)destF;
    for (int i = 0; i < count; ++i) {
        auto s = source + i * srcStride;
        auto d = dest + i * dstStride;
        BFVec4::save(d, BFVec4::load(d) + BFVec4::load(s));
    }
}
static void _MNNDeconvRunForUnitDepthWise(const int16_t* dst, int16_t* src, const int16_t* weight, size_t fw, size_t fh,
                                  size_t weight_y_step, size_t dilateX_step, size_t dilateY_step) {
    int fx, fy;
    auto src_z          = src;
    auto weight_z = weight;
    BFVec4 dstV           = BFVec4::load(dst);
    for (fy = 0; fy < fh; ++fy) {
        auto src_y          = src_z + fy * dilateY_step;
        auto weight_y = weight_z + fy * weight_y_step;
        for (fx = 0; fx < fw; ++fx) {
            BFVec4 weight_x = BFVec4::load(weight_y + 4 * fx);
            BFVec4 src_x    = BFVec4::load(src_y + fx * dilateX_step);
            BFVec4::save(src_y + fx * dilateX_step, src_x + weight_x * dstV);
        }
    }
}
static void _MNNDeconvRunForLineDepthwise(const int16_t* dst, int16_t* src, const int16_t* weight, size_t width, size_t src_w_setup,
                                  size_t fw, size_t fh, size_t dilateX_step, size_t dilateY_step) {
    int dx;
    for (dx = 0; dx < width; ++dx) {
        auto dst_x = dst + dx * 4;
        auto src_dx      = src + src_w_setup * dx;
        _MNNDeconvRunForUnitDepthWise(dst_x, src_dx, weight, fw, fh, fw * 4, dilateX_step, dilateY_step);
    }
}



static CoreFunctions* gInstance = nullptr;
bool BF16Functions::init() {
    gInstance = new CoreFunctions;
    gInstance->MNNConvRunForLineDepthwise = MNNConvRunForLineDepthwiseBF16;
    gInstance->MNNConvRunForUnitDepthWise = MNNConvRunForUnitDepthWiseBF16;
    gInstance->MNNAxByClampBroadcastUnit = MNNAxByClampBroadcastUnitBF16;
    gInstance->MNNFp32ToLowp = _MNNFp32ToLowp;
    gInstance->MNNLowpToFp32 = _MNNLowpToFp32;
    gInstance->bytes = 2;
    gInstance->pack = 4;
    gInstance->MNNPackCUnit = (decltype(gInstance->MNNPackCUnit))MNNPackC4Int16;
    gInstance->MNNUnpackCUnit = (decltype(gInstance->MNNUnpackCUnit))MNNUnpackC4Int16;
    gInstance->MNNUnpackCUnitTranspose = (decltype(gInstance->MNNUnpackCUnitTranspose))MNNPackTransposeInt16;
    gInstance->MNNPackCUnitTranspose = (decltype(gInstance->MNNPackCUnitTranspose))MNNUnpackTransposeInt16;
    gInstance->MNNConvDwF23MulTransUnit = _MNNConvDwF23MulTransUnit;
    gInstance->MNNSourceTransformCommonF23 = _MNNSourceTransformCommonF23;
    gInstance->MNNMultiAndDestTransformCommon23 = _MNNMultiAndDestTransformCommon23;
    gInstance->MNNMatrixAdd = _MNNMatrixAdd;
    gInstance->MNNMatrixSub = _MNNMatrixSub;
    gInstance->MNNStrassenMergeCFunction = _MNNStrassenMergeCFunction;
    gInstance->penalty = 10.0f;
    gInstance->MNNScaleAndAddBias = _MNNScaleAndAddBias;
    gInstance->MNNCopyC4WithStride = MNNCopyC4Int16WithStride;
    gInstance->MNNAddC4WithStride = _MNNAddC4WithStride;
    gInstance->chooseWinoDestTransform = (decltype(gInstance->chooseWinoDestTransform))(WinogradFunctionHalf::chooseDestTransform);
    gInstance->chooseWinoSourceTransform = (decltype(gInstance->chooseWinoSourceTransform))(WinogradFunctionHalf::chooseSourceTransform);
    gInstance->MNNDeconvRunForLineDepthwise = (decltype(gInstance->MNNDeconvRunForLineDepthwise))_MNNDeconvRunForLineDepthwise;
    gInstance->MNNDeconvRunForUnitDepthWise = (decltype(gInstance->MNNDeconvRunForUnitDepthWise))_MNNDeconvRunForUnitDepthWise;

#if !defined(MNN_USE_SSE) && !defined(MNN_USE_NEON)
    gInstance->penalty = 1.5f;
    gInstance->MNNPackForMatMul_B = MNNPackForMatMul_B_BF16; // common function MNNPackForMatMul_B_BF16 is needed even with out sse or arm neon.
    gInstance->MNNPackC4ForMatMul_A = MNNPackC4ForMatMul_A_BF16;//
    gInstance->MNNPackedMatMul = (decltype(gInstance->MNNPackedMatMul))MNNPackedMatMul_BF16;
    gInstance->MNNPackedMatMulRemain = (decltype(gInstance->MNNPackedMatMulRemain))MNNPackedMatMulRemain_BF16;
#endif

#if defined(MNN_USE_SSE)
    gInstance->MNNPackForMatMul_B = _SSE_MNNPackForMatMul_B_BF16;
    auto cpuFlags = libyuv::InitCpuFlags();
    if (!(cpuFlags & libyuv::kCpuHasF16C)) {
        return false;
    }
    if (cpuFlags & libyuv::kCpuHasAVX2) {
        gInstance->MNNPackForMatMul_B = _AVX_MNNPackForMatMul_B_BF16;
        gInstance->MNNGetMatMulPackMode = _AVX_MNNGetMatMulPackMode_BF16;
        gInstance->MNNPackC4ForMatMul_A = _AVX_MNNPackC4ForMatMul_A_BF16;
        gInstance->MNNPackedMatMul = _AVX_MNNPackedMatMulFMA_BF16;
        gInstance->MNNPackedMatMulRemain = _AVX_MNNPackedMatMulRemainFMA_BF16;
        return true;
    }
#elif defined(MNN_USE_NEON)
    gInstance->MNNPackForMatMul_B = NEON_MNNPackForMatMul_B_BF16;
    gInstance->MNNGetMatMulPackMode = NEON_MNNGetMatMulPackMode_BF16;
    gInstance->MNNPackC4ForMatMul_A = NEON_MNNPackC4ForMatMul_A_BF16;
    gInstance->MNNPackedMatMul = NEON_MNNPackedMatMul_BF16;
    gInstance->MNNPackedMatMulRemain = NEON_MNNPackedMatMulRemain_BF16;
    gInstance->MNNConvRunForLineDepthwise = NEON_MNNConvRunForLineDepthwise_BF16;
    gInstance->MNNConvRunForUnitDepthWise = NEON_MNNConvRunForUnitDepthWise_BF16;
    gInstance->MNNAxByClampBroadcastUnit = NEON_MNNAxByClampBroadcastC4_BF16;
    return true;
#endif
    // TODO: raw cpu version of bf16
    return true;
}

CoreFunctions* BF16Functions::get() {
    return gInstance;
}
};
