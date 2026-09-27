// modelpack.cpp
// TachiyomiX 图像增强模型包自带推理库（libmodelpack.so）
//
// 实现 modelpack_abi.h 稳定 C ABI，按 model_key 分发：
//   - "anime4k" : 亮度边缘定向 2x（bloc97/Anime4K Anime4K_Upscale_Original_x2）
//   - "acnet"   : ACNet f8b4 1 通道 CNN（Anime4KCPP Net / ACNetGLSL 同源）
// 两者均为 1 通道亮度 2x 超分；输出 RGBA 必须 = scale * 输入（scale=2）。
//
// 构建：NDK + CMake，ANDROID_STL=c++_static（自包含，不依赖 libc++_shared）。

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "modelpack_abi.h"
#include "acnet_weights_f8b4.h"

#define MODELPACK_SCALE 2

// ---------------------------------------------------------------------------
// 环境状态
// ---------------------------------------------------------------------------

static int g_current_scale = 2;
static int g_tile_size = 128; // ACNet 逐 tile 推理的瓦片边长（读自 options_json）

// 进度回报回调（可选）：modelpack_set_progress_callback 注册，处理线程上触发
static modelpack_progress_fn g_progress_cb = nullptr;

static void report_progress(int pct) {
    modelpack_progress_fn cb = g_progress_cb;
    if (cb) cb(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
}

// ---------------------------------------------------------------------------
// 极简 JSON 数字读取（仅需 "key":value 形式的整数字段）
// ---------------------------------------------------------------------------

static int json_get_int(const char* json, const char* key, int def) {
    if (!json || !key) return def;
    size_t klen = strlen(key);
    const char* p = json;
    while ((p = strstr(p, key)) != NULL) {
        // 确保是独立键：左边是引号
        if (p == json || *(p - 1) == '"') {
            p += klen;
            // 跳过空白与冒号
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '-' || (*p >= '0' && *p <= '9')) {
                    return (int)strtol(p, NULL, 10);
                }
            }
        } else {
            p += klen;
        }
    }
    return def;
}

// ---------------------------------------------------------------------------
// RGB 与亮度转换（BT.601）
// ---------------------------------------------------------------------------

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float clamp01_scale(float x) { return x < 0.f ? 0.f : x; }

static inline float rgb_to_luma(float r, float g, float b) {
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// 双线性采样源图。src 为 RGBA8888，stride 为字节；sx,sy 为像素坐标（可含小数）。
// 坐标越界时按边缘钳制。
static void sample_rgba(const uint8_t* src, int w, int h, int stride,
                        float sx, float sy, float rgb[3]) {
    if (sx < 0.f) sx = 0.f;
    if (sy < 0.f) sy = 0.f;
    if (sx > (float)(w - 1)) sx = (float)(w - 1);
    if (sy > (float)(h - 1)) sy = (float)(h - 1);

    int x0 = (int)sx;
    int y0 = (int)sy;
    if (x0 > w - 1) x0 = w - 1;
    if (y0 > h - 1) y0 = h - 1;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;

    const uint8_t* p00 = src + (size_t)y0 * stride + (size_t)x0 * 4;
    const uint8_t* p10 = src + (size_t)y0 * stride + (size_t)x1 * 4;
    const uint8_t* p01 = src + (size_t)y1 * stride + (size_t)x0 * 4;
    const uint8_t* p11 = src + (size_t)y1 * stride + (size_t)x1 * 4;
    for (int c = 0; c < 3; c++) {
        float w00 = (1.f - fx) * (1.f - fy);
        float w10 = fx * (1.f - fy);
        float w01 = (1.f - fx) * fy;
        float w11 = fx * fy;
        rgb[c] = (p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11) * (1.f / 255.f);
    }
}

// ---------------------------------------------------------------------------
// Anime4K：亮度边缘定向 2x（Anime4K_Upscale_Original_x2）
// ---------------------------------------------------------------------------

// 边锐化多项式（由作者对 MSE 最小二乘拟合得到，GPL-兼容移植）
static inline float anime4k_power_function(float x) {
    const float P5 = 11.68129591f, P4 = -42.46906057f, P3 = 60.28286266f;
    const float P2 = -41.84451327f, P1 = 14.05517353f, P0 = -1.081521930f;
    float x2 = x * x, x3 = x2 * x, x4 = x2 * x2, x5 = x2 * x3;
    return P5 * x5 + P4 * x4 + P3 * x3 + P2 * x2 + P1 * x + P0;
}

static int anime4k_process(const uint8_t* src, int w, int h, int stride,
                           uint8_t* dst, int dstride) {
    const int W2 = w * 2, H2 = h * 2;
    for (int oy = 0; oy < H2; oy++) {
        uint8_t* dstrow = dst + (size_t)oy * dstride;
        // 输出像素在源图上的中心位置
        const float py = (float)oy * 0.5f + 0.25f;
        for (int ox = 0; ox < W2; ox++) {
            const float px = (float)ox * 0.5f + 0.25f;

            // 3x3 亮度邻域用于 Sobel 梯度
            float Y[3][3];
            for (int j = -1; j <= 1; j++) {
                for (int i = -1; i <= 1; i++) {
                    float rgb[3];
                    sample_rgba(src, w, h, stride, px + (float)i, py + (float)j, rgb);
                    Y[i + 1][j + 1] = rgb_to_luma(rgb[0], rgb[1], rgb[2]);
                }
            }
            float gx = -Y[0][0] + Y[2][0] - 2.f * Y[0][1] + 2.f * Y[2][1] - Y[0][2] + Y[2][2];
            float gy = -Y[0][0] - 2.f * Y[1][0] - Y[2][0] + Y[0][2] + 2.f * Y[1][2] + Y[2][2];

            float sobel_norm = sqrtf(gx * gx + gy * gy);
            if (sobel_norm > 1.f) sobel_norm = 1.f;
            float dval = clamp01(anime4k_power_function(sobel_norm) * 0.5f);

            float center[3];
            sample_rgba(src, w, h, stride, px, py, center);

            const float EPS = 0.001f;
            if (dval < 0.1f || sobel_norm <= EPS) {
                // 平坦区：直接输出中心
                dstrow[ox * 4 + 0] = (uint8_t)(center[0] * 255.f + 0.5f);
                dstrow[ox * 4 + 1] = (uint8_t)(center[1] * 255.f + 0.5f);
                dstrow[ox * 4 + 2] = (uint8_t)(center[2] * 255.f + 0.5f);
                dstrow[ox * 4 + 3] = 255;
                continue;
            }

            float nx = gx / sobel_norm;
            float ny = gy / sobel_norm;
            if (fabsf(nx + ny) <= 0.0001f) {
                dstrow[ox * 4 + 0] = (uint8_t)(center[0] * 255.f + 0.5f);
                dstrow[ox * 4 + 1] = (uint8_t)(center[1] * 255.f + 0.5f);
                dstrow[ox * 4 + 2] = (uint8_t)(center[2] * 255.f + 0.5f);
                dstrow[ox * 4 + 3] = 255;
                continue;
            }

            int xpos = nx > 0.f ? -1 : 1; // -sign(nx)
            int ypos = ny > 0.f ? -1 : 1; // -sign(ny)

            float xval[3], yval[3];
            sample_rgba(src, w, h, stride, px + (float)xpos, py, xval);
            sample_rgba(src, w, h, stride, px, py + (float)ypos, yval);

            float ax = fabsf(nx), ay = fabsf(ny);
            float xyratio = ax / (ax + ay);

            float out_rgb[3];
            for (int c = 0; c < 3; c++) {
                float avg = xyratio * xval[c] + (1.f - xyratio) * yval[c];
                out_rgb[c] = avg * dval + center[c] * (1.f - dval);
            }
            dstrow[ox * 4 + 0] = (uint8_t)(out_rgb[0] * 255.f + 0.5f);
            dstrow[ox * 4 + 1] = (uint8_t)(out_rgb[1] * 255.f + 0.5f);
            dstrow[ox * 4 + 2] = (uint8_t)(out_rgb[2] * 255.f + 0.5f);
            dstrow[ox * 4 + 3] = 255;
        }
        // 进度：当前已扫描的输出行占总行数的百分比
        report_progress((int)((oy + 1) * 100 / H2));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ACNet：1 通道 CNN（VGG 风格残差网络），逐 tile 推理
// ---------------------------------------------------------------------------

// 卷积层参数
#define ACNET_CH 8
#define ACNET_K 3

// ACNet 网络有效感受野半径（head + 4 body + upscale 共 6 层 3x3 conv => 6）
static const int ACNet_HALO = 6;

// 3x3 卷积 + 逐通道 PReLU + 可选残差输入（单通道）。对二维图像做 same(pad1) 卷积。
// 输入 in[ic][ih*iw]，输出 out[oc][ih*iw]。
static void conv3x3_float(const float* in, float* out,
                          int oc, int ic, int ih, int iw,
                          const float* wmat, const float* bias, const float* prelu) {
    const int plane = ih * iw;
    for (int o = 0; o < oc; o++) {
        const float* wrow = wmat + (size_t)o * ic * ACNET_K * ACNET_K;
        float* oplane = out + (size_t)o * plane;
        for (int y = 0; y < ih; y++) {
            for (int x = 0; x < iw; x++) {
                float acc = bias[o];
                for (int c = 0; c < ic; c++) {
                    const float* iplane = in + (size_t)c * plane;
                    const float* kw = wrow + (size_t)c * ACNET_K * ACNET_K;
                    for (int ky = 0; ky < ACNET_K; ky++) {
                        int sy = y + ky - 1;
                        if (sy < 0 || sy >= ih) continue;
                        for (int kx = 0; kx < ACNET_K; kx++) {
                            int sx = x + kx - 1;
                            if (sx < 0 || sx >= iw) continue;
                            acc += iplane[(size_t)sy * iw + sx] * kw[kx + ky * ACNET_K];
                        }
                    }
                }
                // PReLU
                float v = acc;
                float ap = prelu ? prelu[o] : 0.f;
                if (v < 0.f) v = ap * v;
                oplane[(size_t)y * iw + x] = v;
            }
        }
    }
}

// 运行整网：输入亮度 Y（浮点 [0..1]，尺寸 pw x ph，已含 halo 填充），
// 输出 out2[2*ph][2*pw] 亮度。输入为 row-major 单通道。
static void acnet_run(const float* y, int pw, int ph, float* out2) {
    // 中间平面
    const size_t plane = (size_t)pw * ph;
    float* head = (float*)malloc(plane * ACNET_CH * sizeof(float));
    float* bufA = (float*)malloc(plane * ACNET_CH * sizeof(float));
    float* bufB = (float*)malloc(plane * ACNET_CH * sizeof(float));
    float* up = (float*)malloc(plane * 4 * sizeof(float));
    if (!head || !bufA || !bufB || !up) {
        free(head); free(bufA); free(bufB); free(up);
        return; // 内存不足，结果未定义（上层会丢页）
    }
    const int ic = 1;

    // head_conv: 1->8 + PReLU, offset 0
    {
        const float* w = ACNET_WEIGHTS + 0;
        const float* b = w + 1 * 8 * 9;      // 72
        const float* a = b + 8;              // 80
        conv3x3_float(y, head, ACNET_CH, ic, ph, pw, w, b, a);
    }

    // 4 层 body_conv: 8->8 + PReLU，offset 88/680/1272/1864
    float* cur = head;
    const static int body_offset[4] = { 88, 680, 1272, 1864 };
    for (int blk = 0; blk < 4; blk++) {
        const float* w = ACNET_WEIGHTS + body_offset[blk];
        const float* b = w + 8 * 8 * 9;      // 576
        const float* a = b + 8;              // 584
        float* nxt = (blk % 2 == 0) ? bufA : bufB;
        conv3x3_float(cur, nxt, ACNET_CH, ACNET_CH, ph, pw, w, b, a);
        if (blk < 3) cur = nxt; // 最后一次之后 cur 不再使用
        if (blk == 3) cur = nxt;
    }

    // upscale_conv: 8->4（无 PReLU），offset 2456
    {
        const float* w = ACNET_WEIGHTS + 2456;
        const float* b = w + 8 * 4 * 9;      // 288
        for (int o = 0; o < 4; o++) {
            float* oplane = up + (size_t)o * plane;
            const float* wrow = w + (size_t)o * 8 * 9;
            for (int py = 0; py < ph; py++) {
                for (int px = 0; px < pw; px++) {
                    float acc = b[o];
                    for (int c = 0; c < ACNET_CH; c++) {
                        const float* iplane = cur + (size_t)c * plane;
                        const float* kw = wrow + (size_t)c * 9;
                        for (int ky = 0; ky < ACNET_K; ky++) {
                            int sy = py + ky - 1;
                            if (sy < 0 || sy >= ph) continue;
                            for (int kx = 0; kx < ACNET_K; kx++) {
                                int sx = px + kx - 1;
                                if (sx < 0 || sx >= pw) continue;
                                acc += iplane[(size_t)sy * pw + sx] * kw[kx + ky * ACNET_K];
                            }
                        }
                    }
                    // 残差：加上输入亮度
                    float v = acc + y[(size_t)py * pw + px];
                    up[(size_t)o * plane + (size_t)py * pw + px] = clamp01(v);
                }
            }
        }
    }

    // depth-to-space 2x：通道 (dy*2+dx) -> 坐标 (2x+dx, 2y+dy)
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int ch = dy * 2 + dx;
                    float v = up[(size_t)ch * plane + (size_t)y * pw + x];
                    out2[((size_t)(2 * y + dy) * (2 * pw)) + (2 * x + dx)] = v;
                }
            }
        }
    }

    free(head);
    free(bufA);
    free(bufB);
    free(up);
}

static int acnet_process(const uint8_t* src, int w, int h, int stride,
                         uint8_t* dst, int dstride) {
    const int W2 = w * 2, H2 = h * 2;
    const int ts = g_tile_size > 0 ? g_tile_size : 128;
    const int hal = ACNet_HALO;

    // 源亮度平面
    float* y = (float*)malloc((size_t)w * h * sizeof(float));
    if (!y) return -1;
    for (int yy = 0; yy < h; yy++) {
        const uint8_t* row = src + (size_t)yy * stride;
        for (int xx = 0; xx < w; xx++) {
            y[(size_t)yy * w + xx] =
                (0.299f * row[xx * 4] + 0.587f * row[xx * 4 + 1] + 0.114f * row[xx * 4 + 2]) * (1.f / 255.f);
        }
    }

    // 输出亮度平面
    float* y2 = (float*)calloc((size_t)W2 * H2, sizeof(float));
    if (!y2) { free(y); return -1; }

    // 逐 tile，tile 含 halo 填充
    const int total_tiles = ((h + ts - 1) / ts) * ((w + ts - 1) / ts);
    int done_tiles = 0;
    for (int ty0 = 0; ty0 < h; ty0 += ts) {
        int th = (ty0 + ts < h) ? ts : (h - ty0);
        for (int tx0 = 0; tx0 < w; tx0 += ts) {
            int tw = (tx0 + ts < w) ? ts : (w - tx0);

            int pw = tw + 2 * hal; // 带 halo 的 tile 宽
            int ph = th + 2 * hal;
            float* tile = (float*)malloc((size_t)pw * ph * sizeof(float));
            float* tile2 = (float*)malloc((size_t)(2 * pw) * (2 * ph) * sizeof(float));
            if (!tile || !tile2) {
                free(tile); free(tile2); free(y); free(y2); return -1;
            }

            // 填充 halo 平面（越界钳制）
            for (int yy = 0; yy < ph; yy++) {
                int sy = ty0 - hal + yy;
                if (sy < 0) sy = 0;
                if (sy > h - 1) sy = h - 1;
                const float* srow = y + (size_t)sy * w;
                for (int xx = 0; xx < pw; xx++) {
                    int sx = tx0 - hal + xx;
                    if (sx < 0) sx = 0;
                    if (sx > w - 1) sx = w - 1;
                    tile[(size_t)yy * pw + xx] = srow[sx];
                }
            }

            acnet_run(tile, pw, ph, tile2);

            // 拷回有效区域（halo 由 tile 中心 2x 区域写入 y2）。
            // 每个输入行放大为 2 行，两行都要写回，否则奇数输出行残留为 0（黑色横条纹）。
            for (int dy = 0; dy < th; dy++) {
                int sy2 = 2 * (ty0 + dy);
                for (int sub = 0; sub < 2; sub++) {
                    // tile2 中对应行：halo 2*hal 开始，每输入行占 2 个输出行
                    const float* srow = tile2 + (size_t)(2 * (hal + dy) + sub) * (2 * pw) + 2 * hal;
                    float* drows = y2 + (size_t)(sy2 + sub) * W2 + 2 * tx0;
                    memcpy(drows, srow, (size_t)tw * 2 * sizeof(float));
                }
            }

            free(tile);
            free(tile2);

            // 进度：当前已推理完成的 tile 占全部 tile 的百分比
            done_tiles++;
            report_progress((int)(done_tiles * 100 / total_tiles));
        }
    }

    // 色度保持源像素色相，仅用 ACNet 2x 亮度重新加权亮度，实现锐化且不引入色偏
    for (int oy = 0; oy < H2; oy++) {
        uint8_t* drows = dst + (size_t)oy * dstride;
        int sy = oy / 2;
        const uint8_t* p = src + (size_t)sy * stride;
        for (int ox = 0; ox < W2; ox++) {
            int sx = ox / 2;
            // ACNet 亮度
            float luma = y2[(size_t)oy * W2 + ox];
            float r = p[sx * 4] * (1.f / 255.f);
            float g = p[sx * 4 + 1] * (1.f / 255.f);
            float b = p[sx * 4 + 2] * (1.f / 255.f);
            float old_luma = rgb_to_luma(r, g, b);
            float scale_l = (old_luma > 1e-5f) ? clamp01_scale(luma / old_luma) : 1.f;

            drows[ox * 4 + 0] = (uint8_t)(r * scale_l * 255.f + 0.5f);
            drows[ox * 4 + 1] = (uint8_t)(g * scale_l * 255.f + 0.5f);
            drows[ox * 4 + 2] = (uint8_t)(b * scale_l * 255.f + 0.5f);
            drows[ox * 4 + 3] = 255;
        }
    }

    free(y);
    free(y2);
    return 0;
}

// ---------------------------------------------------------------------------
// modelpack ABI
// ---------------------------------------------------------------------------

extern "C" {

int modelpack_abi_version(void) { return MODELPACK_ABI_VERSION; }

const char* modelpack_describe(void) {
    return "{\"schemaVersion\":1,\"id\":\"anime4k-acnet\",\"models\":["
           "{\"key\":\"anime4k\",\"name\":\"Anime4K edge-directed 2x\",\"scales\":[2]},"
           "{\"key\":\"acnet\",\"name\":\"ACNet f8b4 2x\",\"scales\":[2]}]}";
}

// 当前模型 key
static char g_model_key[32] = {0};

// 单次 2x 核心超分（acnet / anime4k）到密集缓冲
static void run_2x(const uint8_t* src, int w, int h, int sstride,
                   uint8_t* dst, int dstride) {
    if (strcmp(g_model_key, "acnet") == 0) {
        acnet_process(src, w, h, sstride, dst, dstride);
    } else {
        anime4k_process(src, w, h, sstride, dst, dstride);
    }
}

int modelpack_init(const char* model_key, const char* options_json) {
    if (!model_key) return -1;
    size_t n = strlen(model_key);
    if (n == 0 || n >= sizeof(g_model_key)) return -1;
    memcpy(g_model_key, model_key, n + 1);

    g_current_scale = json_get_int(options_json, "scale", 2);
    int ts = json_get_int(options_json, "tileSize", 0);
    g_tile_size = (ts > 0) ? ts : 128;

    bool ok_anime4k = strcmp(g_model_key, "anime4k") == 0;
    bool ok_acnet = strcmp(g_model_key, "acnet") == 0;
    if (!ok_anime4k && !ok_acnet) {
        fprintf(stderr, "[modelpack] unknown model_key=%s\n", g_model_key);
        return -1;
    }
    return 0;
}

int modelpack_process(const unsigned char* rgba_in,
                      int width, int height, int stride_in,
                      unsigned char* rgba_out, int stride_out) {
    if (!rgba_in || !rgba_out || width < 1 || height < 1) return -1;
    if (g_current_scale != 2) return -1; // 仅支持 2x
    report_progress(0);
    run_2x(rgba_in, width, height, stride_in, rgba_out, stride_out);
    report_progress(100);
    return 0;
}

void modelpack_release(void) {
    // 无持久资源
}

void modelpack_set_progress_callback(modelpack_progress_fn progress) {
    g_progress_cb = progress; // 由宿主在 init 前注册；回调在 process 线程触发
}

} // extern "C"