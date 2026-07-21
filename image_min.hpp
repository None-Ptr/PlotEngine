// image_min.hpp — PlotEngine 零依赖终端图片渲染
// ---------------------------------------------------------------------------
// 提供: 自包含 DEFLATE 解压(PNG) + BMP 解析 + tiv 半块字符渲染算法 + FTXUI 节点
// 不链接任何外部库(zlib/libpng/...), 静态 exe 自包含。
// 渲染算法移植自 ftxui-image-view / TerminalImageViewer (Apache-2.0),
// 采用 "4x8 像素块 -> Unicode 区块字符 + 24-bit 真彩" 方案。
// DEFLATE 解压采用 tinf (tiny inflate, 公有领域, Joergen Ibsen, tinf 1.2.1),
// 自包含、无外部依赖。
// 维护方式: PlotEngine.cpp 用 #include "image_min.hpp"; 发布时把本文件内容
// 按依赖顺序(ftxui 之后, 应用代码之前)内联进 plotengine_release.cpp。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <cassert>
#include <algorithm>
#include <memory>

// ===========================================================================
// 图像缓冲: 统一为 8-bit RGB, 3 字节/像素
// ===========================================================================
struct ImgBuf {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgb;  // 长度 = w*h*3
};

// tiv 渲染结果: 一个终端字符单元的前/背景色与码点
struct CharData {
    int fg[3] = {0, 0, 0};              // 前景 RGB
    int bg[3] = {0, 0, 0};              // 背景 RGB
    unsigned int codepoint = 0x2584;   // Unicode 码点 (默认 ▄)
};

// 全局图片注册表: ID -> 原始 PNG/BMP 字节。由 PlotEngine.cpp 在 loadScript
// 阶段填充/清空; ImageNode 在渲染时按 id 查找并解码。
extern std::map<std::string, std::vector<unsigned char>> g_images;

// ===========================================================================
// 自包含 DEFLATE (RFC 1951) 解压 —— tinf (tiny inflate), 公有领域, tinf 1.2.1
// 仅处理 raw DEFLATE 流, 供 PNG 的 zlib/IDAT 使用(调用方跳过 2 字节 zlib 头)。
// ===========================================================================
namespace peimg {

struct tinf_tree {
    unsigned short counts[16];
    unsigned short symbols[288];
    int max_sym;
};
struct tinf_data {
    const unsigned char *source;
    const unsigned char *source_end;
    unsigned int tag;
    int bitcount;
    int overflow;
    unsigned char *dest_start;
    unsigned char *dest;
    unsigned char *dest_end;
    struct tinf_tree ltree;
    struct tinf_tree dtree;
};

static unsigned int read_le16(const unsigned char *p) {
    return ((unsigned int)p[0]) | ((unsigned int)p[1] << 8);
}

static void tinf_build_fixed_trees(struct tinf_tree *lt, struct tinf_tree *dt) {
    int i;
    for (i = 0; i < 16; ++i) lt->counts[i] = 0;
    lt->counts[7] = 24;
    lt->counts[8] = 152;
    lt->counts[9] = 112;
    for (i = 0; i < 24; ++i) lt->symbols[i] = 256 + i;
    for (i = 0; i < 144; ++i) lt->symbols[24 + i] = i;
    for (i = 0; i < 8; ++i) lt->symbols[24 + 144 + i] = 280 + i;
    for (i = 0; i < 112; ++i) lt->symbols[24 + 144 + 8 + i] = 144 + i;
    lt->max_sym = 285;
    for (i = 0; i < 16; ++i) dt->counts[i] = 0;
    dt->counts[5] = 32;
    for (i = 0; i < 32; ++i) dt->symbols[i] = i;
    dt->max_sym = 29;
}

static int tinf_build_tree(struct tinf_tree *t, const unsigned char *lengths,
                           unsigned int num) {
    unsigned short offs[16];
    unsigned int i, num_codes, available;
    for (i = 0; i < 16; ++i) t->counts[i] = 0;
    t->max_sym = -1;
    for (i = 0; i < num; ++i) {
        assert(lengths[i] <= 15);
        if (lengths[i]) {
            t->max_sym = i;
            t->counts[lengths[i]]++;
        }
    }
    for (available = 1, num_codes = 0, i = 0; i < 16; ++i) {
        unsigned int used = t->counts[i];
        if (used > available) return -3;
        available = 2 * (available - used);
        offs[i] = num_codes;
        num_codes += used;
    }
    if ((num_codes > 1 && available > 0) ||
        (num_codes == 1 && t->counts[1] != 1))
        return -3;
    for (i = 0; i < num; ++i) {
        if (lengths[i]) t->symbols[offs[lengths[i]]++] = i;
    }
    if (num_codes == 1) {
        t->counts[1] = 2;
        t->symbols[1] = t->max_sym + 1;
    }
    return 0;
}

static void tinf_refill(struct tinf_data *d, int num) {
    assert(num >= 0 && num <= 32);
    while (d->bitcount < num) {
        if (d->source != d->source_end) {
            d->tag |= (unsigned int)*d->source++ << d->bitcount;
        } else {
            d->overflow = 1;
        }
        d->bitcount += 8;
    }
    assert(d->bitcount <= 32);
}

static unsigned int tinf_getbits_no_refill(struct tinf_data *d, int num) {
    unsigned int bits;
    assert(num >= 0 && num <= d->bitcount);
    bits = d->tag & ((1UL << num) - 1);
    d->tag >>= num;
    d->bitcount -= num;
    return bits;
}

static unsigned int tinf_getbits(struct tinf_data *d, int num) {
    tinf_refill(d, num);
    return tinf_getbits_no_refill(d, num);
}

static unsigned int tinf_getbits_base(struct tinf_data *d, int num, int base) {
    return base + (num ? (int)tinf_getbits(d, num) : 0);
}

static int tinf_decode_symbol(struct tinf_data *d, const struct tinf_tree *t) {
    int base = 0, offs = 0;
    int len;
    for (len = 1;; ++len) {
        offs = 2 * offs + (int)tinf_getbits(d, 1);
        assert(len <= 15);
        if (offs < t->counts[len]) break;
        base += t->counts[len];
        offs -= t->counts[len];
    }
    assert(base + offs >= 0 && base + offs < 288);
    return t->symbols[base + offs];
}

static int tinf_decode_trees(struct tinf_data *d, struct tinf_tree *lt,
                             struct tinf_tree *dt) {
    unsigned char lengths[288 + 32];
    static const unsigned char clcidx[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                             11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned int hlit, hdist, hclen;
    unsigned int i, num, length;
    int res;
    hlit = tinf_getbits_base(d, 5, 257);
    hdist = tinf_getbits_base(d, 5, 1);
    hclen = tinf_getbits_base(d, 4, 4);
    if (hlit > 286 || hdist > 30) return -3;
    for (i = 0; i < 19; ++i) lengths[i] = 0;
    for (i = 0; i < hclen; ++i)
        lengths[clcidx[i]] = (unsigned char)tinf_getbits(d, 3);
    res = tinf_build_tree(lt, lengths, 19);
    if (res != 0) return res;
    if (lt->max_sym == -1) return -3;
    for (num = 0; num < hlit + hdist;) {
        int sym = tinf_decode_symbol(d, lt);
        if (sym > lt->max_sym) return -3;
        switch (sym) {
            case 16:
                if (num == 0) return -3;
                sym = lengths[num - 1];
                length = tinf_getbits_base(d, 2, 3);
                break;
            case 17:
                sym = 0;
                length = tinf_getbits_base(d, 3, 3);
                break;
            case 18:
                sym = 0;
                length = tinf_getbits_base(d, 7, 11);
                break;
            default:
                length = 1;
                break;
        }
        if (length > hlit + hdist - num) return -3;
        while (length--) lengths[num++] = (unsigned char)sym;
    }
    if (lengths[256] == 0) return -3;
    res = tinf_build_tree(lt, lengths, hlit);
    if (res != 0) return res;
    res = tinf_build_tree(dt, lengths + hlit, hdist);
    if (res != 0) return res;
    return 0;
}

static int tinf_inflate_block_data(struct tinf_data *d, struct tinf_tree *lt,
                                   struct tinf_tree *dt) {
    static const unsigned char length_bits[30] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0};
    static const unsigned short length_base[30] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0};
    static const unsigned char dist_bits[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static const unsigned short dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
        12289, 16385, 24577};
    for (;;) {
        int sym = tinf_decode_symbol(d, lt);
        if (d->overflow) return -3;
        if (sym < 256) {
            if (d->dest == d->dest_end) return -5;
            *d->dest++ = (unsigned char)sym;
        } else {
            int length, dist, offs, i;
            if (sym == 256) return 0;
            if (sym > lt->max_sym || sym - 257 > 28 || dt->max_sym == -1)
                return -3;
            sym -= 257;
            length = tinf_getbits_base(d, length_bits[sym], length_base[sym]);
            dist = tinf_decode_symbol(d, dt);
            if (dist > dt->max_sym || dist > 29) return -3;
            offs = tinf_getbits_base(d, dist_bits[dist], dist_base[dist]);
            if (offs > d->dest - d->dest_start) return -3;
            if (d->dest_end - d->dest < length) return -5;
            for (i = 0; i < length; ++i) d->dest[i] = d->dest[i - offs];
            d->dest += length;
        }
    }
}

static int tinf_inflate_uncompressed_block(struct tinf_data *d) {
    unsigned int length, invlength;
    if (d->source_end - d->source < 4) return -3;
    length = read_le16(d->source);
    invlength = read_le16(d->source + 2);
    if (length != (~invlength & 0x0000FFFF)) return -3;
    d->source += 4;
    if (d->source_end - d->source < length) return -3;
    if (d->dest_end - d->dest < length) return -5;
    while (length--) *d->dest++ = *d->source++;
    d->tag = 0;
    d->bitcount = 0;
    return 0;
}

static int tinf_inflate_fixed_block(struct tinf_data *d) {
    tinf_build_fixed_trees(&d->ltree, &d->dtree);
    return tinf_inflate_block_data(d, &d->ltree, &d->dtree);
}

static int tinf_inflate_dynamic_block(struct tinf_data *d) {
    int res = tinf_decode_trees(d, &d->ltree, &d->dtree);
    if (res != 0) return res;
    return tinf_inflate_block_data(d, &d->ltree, &d->dtree);
}

static int tinf_uncompress(void *dest, unsigned int *destLen,
                           const void *source, unsigned int sourceLen) {
    struct tinf_data d;
    int bfinal;
    d.source = (const unsigned char *)source;
    d.source_end = d.source + sourceLen;
    d.tag = 0;
    d.bitcount = 0;
    d.overflow = 0;
    d.dest = (unsigned char *)dest;
    d.dest_start = d.dest;
    d.dest_end = d.dest + *destLen;
    do {
        unsigned int btype;
        int res;
        bfinal = (int)tinf_getbits(&d, 1);
        btype = tinf_getbits(&d, 2);
        switch (btype) {
            case 0:
                res = tinf_inflate_uncompressed_block(&d);
                break;
            case 1:
                res = tinf_inflate_fixed_block(&d);
                break;
            case 2:
                res = tinf_inflate_dynamic_block(&d);
                break;
            default:
                res = -3;
                break;
        }
        if (res != 0) return res;
    } while (!bfinal);
    if (d.overflow) return -3;
    *destLen = (unsigned int)(d.dest - d.dest_start);
    return 0;
}

// 包装: 解压 raw DEFLATE 流, 输出到 std::vector。maxOut 为预期解压大小上限。
static bool inflate(const unsigned char *src, size_t srclen,
                    std::vector<unsigned char> &out, size_t maxOut) {
    if (srclen == 0) return false;
    std::vector<unsigned char> buf(maxOut + 1024);
    unsigned int destLen = (unsigned int)buf.size();
    int r = tinf_uncompress(buf.data(), &destLen, src, (unsigned int)srclen);
    if (r != 0) return false;
    out.assign(buf.begin(), buf.begin() + destLen);
    return true;
}

// ===========================================================================
// PNG 解码 (仅支持 8-bit: 灰度/灰度+α/RGB/RGBA/调色板)
// ===========================================================================
static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static bool png_decode(const std::vector<unsigned char> &raw, ImgBuf &out) {
    if (raw.size() < 8 || raw[0] != 0x89 || raw[1] != 0x50 || raw[2] != 0x4E ||
        raw[3] != 0x47)
        return false;
    size_t p = 8;
    int width = 0, height = 0, bitdepth = 0, colortype = 0;
    std::vector<unsigned char> idat;
    std::vector<unsigned char> palette;
    bool hasIHDR = false;
    while (p + 8 <= raw.size()) {
        uint32_t len =
            ((uint32_t)raw[p] << 24) | ((uint32_t)raw[p + 1] << 16) |
            ((uint32_t)raw[p + 2] << 8) | (uint32_t)raw[p + 3];
        std::string type((const char *)&raw[p + 4], 4);
        const unsigned char *data = &raw[p + 8];
        if (type == "IHDR") {
            width = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            height =
                (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
            bitdepth = data[8];
            colortype = data[9];
            hasIHDR = true;
        } else if (type == "PLTE") {
            if ((size_t)len <= raw.size() - (p + 8))
                palette.assign(data, data + len);
        } else if (type == "IDAT") {
            if ((size_t)len <= raw.size() - (p + 8))
                idat.insert(idat.end(), data, data + len);
        } else if (type == "IEND") {
            break;
        }
        size_t adv = 8 + (size_t)len + 4;
        if (adv > raw.size() - p) break;
        p += adv;
    }
    if (!hasIHDR || width <= 0 || height <= 0) return false;
    if (bitdepth != 8) return false;
    int channels = (colortype == 2)   ? 3
                   : (colortype == 6) ? 4
                   : (colortype == 0) ? 1
                   : (colortype == 4) ? 2
                   : (colortype == 3) ? 1
                                      : 0;
    if (channels == 0) return false;

    if (idat.size() < 2) return false;
    // 跳过 zlib 2 字节头(CMF/FLG), 末尾 4 字节 adler32 由 tinf 自然忽略
    std::vector<unsigned char> zdata;
    size_t maxOut = (size_t)height * (1 + (size_t)width * channels);
    if (!inflate(&idat[2], idat.size() - 2, zdata, maxOut)) return false;

    int stride = width * channels;
    out.w = width;
    out.h = height;
    out.rgb.clear();
    out.rgb.reserve((size_t)width * height * 3);
    std::vector<unsigned char> cur(stride), prev(stride, 0);
    size_t pos = 0;
    for (int y = 0; y < height; y++) {
        if (pos >= zdata.size()) return false;
        int filter = zdata[pos++];
        for (int i = 0; i < stride; i++) {
            int x = (pos < zdata.size()) ? zdata[pos++] : 0;
            int a = (i >= channels) ? cur[i - channels] : 0;
            int b = prev[i];
            int c = (i >= channels) ? prev[i - channels] : 0;
            int val;
            switch (filter) {
                case 0: val = x; break;
                case 1: val = x + a; break;
                case 2: val = x + b; break;
                case 3: val = x + ((a + b) >> 1); break;
                case 4: val = x + paeth(a, b, c); break;
                default: return false;
            }
            cur[i] = (unsigned char)(val & 0xFF);
        }
        for (int xx = 0; xx < width; xx++) {
            int base = xx * channels;
            int r, g, bl;
            if (colortype == 2 || colortype == 6) {
                r = cur[base];
                g = cur[base + 1];
                bl = cur[base + 2];
            } else if (colortype == 0 || colortype == 4) {
                r = g = bl = cur[base];
            } else {
                int idx = cur[base];
                if (idx * 3 + 2 < (int)palette.size()) {
                    r = palette[idx * 3];
                    g = palette[idx * 3 + 1];
                    bl = palette[idx * 3 + 2];
                } else {
                    r = g = bl = 0;
                }
            }
            out.rgb.push_back((unsigned char)r);
            out.rgb.push_back((unsigned char)g);
            out.rgb.push_back((unsigned char)bl);
        }
        prev = cur;
    }
    return true;
}

// ===========================================================================
// BMP 解码 (仅支持未压缩 24/32-bit, 含上下翻转修正)
// ===========================================================================
static bool bmp_decode(const std::vector<unsigned char> &raw, ImgBuf &out) {
    if (raw.size() < 54) return false;
    if (raw[0] != 'B' || raw[1] != 'M') return false;
    int offset =
        raw[10] | (raw[11] << 8) | (raw[12] << 16) | (raw[13] << 24);
    int w = raw[18] | (raw[19] << 8) | (raw[20] << 16) | (raw[21] << 24);
    int h = raw[22] | (raw[23] << 8) | (raw[24] << 16) | (raw[25] << 24);
    int bpp = raw[28] | (raw[29] << 8);
    int comp =
        raw[30] | (raw[31] << 8) | (raw[32] << 16) | (raw[33] << 24);
    if (comp != 0) return false;
    if (bpp != 24 && bpp != 32) return false;
    if (w <= 0 || h <= 0) return false;
    int bppBytes = bpp / 8;
    int rowBytes = ((w * bppBytes) + 3) & ~3;
    out.w = w;
    out.h = h;
    out.rgb.clear();
    out.rgb.reserve((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        int srcRow = h - 1 - y;
        size_t rowPos = (size_t)offset + (size_t)srcRow * rowBytes;
        for (int x = 0; x < w; x++) {
            size_t pp = rowPos + (size_t)x * bppBytes;
            if (pp + 2 >= raw.size()) {
                out.rgb.push_back(0);
                out.rgb.push_back(0);
                out.rgb.push_back(0);
                continue;
            }
            unsigned char b = raw[pp], g = raw[pp + 1], r = raw[pp + 2];
            out.rgb.push_back(r);
            out.rgb.push_back(g);
            out.rgb.push_back(b);
        }
    }
    return true;
}

// ===========================================================================
// tiv 渲染: 4x8 像素块 -> 最佳 Unicode 区块字符 + 双色
// ===========================================================================
static int popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (int)((v * 0x01010101u) >> 24);
}

struct BmpEnt {
    uint32_t pat;        // 4x8 网格位掩码, MSB = 左上(x=0,y=0)
    unsigned int cp;     // Unicode 码点
};

// 位图: 1/8 竖条 + 半块 + 四分块 + 三角/对角组合 (精确计算, 见各字符定义)
static const BmpEnt BITMAPS[] = {
    {0x0000000Fu, 0x2581u},  // ▁ 下 1/8
    {0x000000FFu, 0x2582u},  // ▂ 下 1/4
    {0x00000FFFu, 0x2583u},  // ▃ 下 3/8
    {0x0000FFFFu, 0x2584u},  // ▄ 下 1/2
    {0x000FFFFFu, 0x2585u},  // ▅ 下 5/8
    {0x00FFFFFFu, 0x2586u},  // ▆ 下 3/4
    {0x0FFFFFFFu, 0x2587u},  // ▇ 下 7/8
    {0xFFFFFFFFu, 0x2588u},  // █ 满块
    {0xFFFF0000u, 0x2580u},  // ▀ 上 1/2
    {0xCCCCCCCCu, 0x258Cu},  // ▌ 左 1/2
    {0x33333333u, 0x2590u},  // ▐ 右 1/2
    {0xCCCC0000u, 0x2598u},  // ▘ 左上
    {0x33330000u, 0x259Du},  // ▝ 右上
    {0x0000CCCCu, 0x2596u},  // ▖ 左下
    {0x00003333u, 0x2597u},  // ▗ 右下
    {0xCCCCFFFFu, 0x2599u},  // ▙ 左三象限
    {0xFFFFCCCCu, 0x259Bu},  // ▛ 上 + 左下
    {0xFFFF3333u, 0x259Cu},  // ▜ 上 + 右下
    {0x3333FFFFu, 0x259Fu},  // ▟ 下 + 右上
    {0xCCCC3333u, 0x259Eu},  // ▞ 左上 + 右下
    {0x3333CCCCu, 0x259Au},  // ▚ 右上 + 左下
};

// samp: 4(宽) x 8(高) x 3(RGB) 采样块
static void tiv_find(const int samp[4][8][3], CharData &cd) {
    int rmin[3] = {255, 255, 255}, rmax[3] = {0, 0, 0};
    struct Col { int r, g, b, c; };
    std::vector<Col> hist;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 4; x++) {
            int r = samp[x][y][0], g = samp[x][y][1], b = samp[x][y][2];
            if (r < rmin[0]) rmin[0] = r;
            if (r > rmax[0]) rmax[0] = r;
            if (g < rmin[1]) rmin[1] = g;
            if (g > rmax[1]) rmax[1] = g;
            if (b < rmin[2]) rmin[2] = b;
            if (b > rmax[2]) rmax[2] = b;
            bool found = false;
            for (size_t k = 0; k < hist.size(); k++) {
                if (hist[k].r == r && hist[k].g == g && hist[k].b == b) {
                    hist[k].c++;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Col e = {r, g, b, 1};
                hist.push_back(e);
            }
        }
    Col c1 = {0, 0, 0, 0}, c2 = {0, 0, 0, 0};
    for (size_t k = 0; k < hist.size(); k++) {
        if (hist[k].c > c1.c) {
            c2 = c1;
            c1 = hist[k];
        } else if (hist[k].c > c2.c) {
            c2 = hist[k];
        }
    }
    uint32_t bits = 0;
    int fg[3] = {0, 0, 0}, bg[3] = {0, 0, 0};
    bool direct = (c1.c + c2.c) > 16;
    if (direct) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 4; x++) {
                int r = samp[x][y][0], g = samp[x][y][1], b = samp[x][y][2];
                int d1 = (r - c1.r) * (r - c1.r) + (g - c1.g) * (g - c1.g) +
                         (b - c1.b) * (b - c1.b);
                int d2 = (r - c2.r) * (r - c2.r) + (g - c2.g) * (g - c2.g) +
                         (b - c2.b) * (b - c2.b);
                int on = (d2 < d1) ? 1 : 0;
                bits |= ((uint32_t)on << (31 - (y * 4 + x)));
            }
        fg[0] = c1.r; fg[1] = c1.g; fg[2] = c1.b;
        bg[0] = c2.r; bg[1] = c2.g; bg[2] = c2.b;
    } else {
        int ch = 0;
        if (rmax[1] - rmin[1] > rmax[ch] - rmin[ch]) ch = 1;
        if (rmax[2] - rmin[2] > rmax[ch] - rmin[ch]) ch = 2;
        int split = (rmin[ch] + rmax[ch]) / 2;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 4; x++) {
                int v = samp[x][y][ch];
                int on = (v > split) ? 1 : 0;
                bits |= ((uint32_t)on << (31 - (y * 4 + x)));
            }
    }
    uint32_t best = 0xFFFFFFFFu;
    unsigned int bestcp = 0x2584u;
    bool bestinv = false;
    for (size_t i = 0; i < sizeof(BITMAPS) / sizeof(BITMAPS[0]); i++) {
        uint32_t d = popcount32(bits ^ BITMAPS[i].pat);
        if (d < best) {
            best = d;
            bestcp = BITMAPS[i].cp;
            bestinv = false;
        }
        uint32_t di = popcount32(bits ^ (~BITMAPS[i].pat));
        if (di < best) {
            best = di;
            bestcp = BITMAPS[i].cp;
            bestinv = true;
        }
    }
    if (direct) {
        if (bestinv) {
            int t;
            for (int i = 0; i < 3; i++) {
                t = fg[i]; fg[i] = bg[i]; bg[i] = t;
            }
        }
        cd.fg[0] = fg[0]; cd.fg[1] = fg[1]; cd.fg[2] = fg[2];
        cd.bg[0] = bg[0]; cd.bg[1] = bg[1]; cd.bg[2] = bg[2];
        cd.codepoint = bestcp;
    } else {
        uint32_t pat = bestinv ? (~bits) : bits;
        int fgs[3] = {0, 0, 0}, bgs[3] = {0, 0, 0};
        int fgn = 0, bgn = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 4; x++) {
                int on = (int)((pat >> (31 - (y * 4 + x))) & 1u);
                int r = samp[x][y][0], g = samp[x][y][1], b = samp[x][y][2];
                if (on) {
                    fgs[0] += r; fgs[1] += g; fgs[2] += b; fgn++;
                } else {
                    bgs[0] += r; bgs[1] += g; bgs[2] += b; bgn++;
                }
            }
        cd.fg[0] = fgn ? fgs[0] / fgn : 0;
        cd.fg[1] = fgn ? fgs[1] / fgn : 0;
        cd.fg[2] = fgn ? fgs[2] / fgn : 0;
        cd.bg[0] = bgn ? bgs[0] / bgn : 0;
        cd.bg[1] = bgn ? bgs[1] / bgn : 0;
        cd.bg[2] = bgn ? bgs[2] / bgn : 0;
        cd.codepoint = bestcp;
    }
}

// Unicode 码点 -> UTF-8 字符串
static std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back((char)cp);
    } else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (cp >> 18)));
        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
    return s;
}

}  // namespace peimg

// ===========================================================================
// FTXUI 自定义节点: 把图片渲染进屏幕像素缓冲
// ===========================================================================
struct ImageNode : ftxui::Node {
    std::string id;
    std::string mode;   // "quarter"(默认) | "half"
    ImgBuf img;
    bool decoded = false;
    bool ok = false;

    ImageNode(std::string id_, std::string mode_ = "quarter")
        : id(std::move(id_)), mode(std::move(mode_)) {}

    void decode() {
        decoded = true;
        auto it = g_images.find(id);
        if (it == g_images.end()) {
            ok = false;
            return;
        }
        const std::vector<unsigned char> &raw = it->second;
        if (raw.size() >= 8 && raw[0] == 0x89 && raw[1] == 0x50 &&
            raw[2] == 0x4E && raw[3] == 0x47)
            ok = peimg::png_decode(raw, img);
        else if (raw.size() >= 2 && raw[0] == 'B' && raw[1] == 'M')
            ok = peimg::bmp_decode(raw, img);
        else
            ok = false;
    }

    void ComputeRequirement() override {
        if (!decoded) decode();
        if (mode == "half") {
            // half (半块): 每格覆盖 4 宽 x 8 高 像素
            requirement_.min_x = img.w > 0 ? (img.w + 3) / 4 : 1;
            requirement_.min_y = img.h > 0 ? (img.h + 7) / 8 : 1;
        } else {  // quarter (默认): 每格覆盖 2 宽 x 2 高 像素
            requirement_.min_x = img.w > 0 ? (img.w + 1) / 2 : 1;
            requirement_.min_y = img.h > 0 ? (img.h + 1) / 2 : 1;
        }
    }

    void SetBox(ftxui::Box box) override { box_ = box; }

    void Render(ftxui::Screen &screen) override {
        if (!decoded) decode();
        int CW = box_.x_max - box_.x_min + 1;
        int CH = box_.y_max - box_.y_min + 1;
        if (CW <= 0 || CH <= 0) return;
        if (!ok) {
            ftxui::Pixel p;
            p.character = "?";
            p.foreground_color = ftxui::Color::Red;
            screen.PixelAt(box_.x_min, box_.y_min) = p;
            return;
        }
        // 保持图片原始纵横比: 终端字符高:宽≈2:1。
        // 不拉伸条件: 渲染列数/行数 = 2*w/h (各模式共用, 仅子像素采样密度不同)。
        double ratio = 2.0 * img.w / img.h;  // 每行对应的列数
        int rows = CH;
        int cols = (int)(rows * ratio + 0.5);
        if (cols > CW) { cols = CW; rows = (int)(cols / ratio + 0.5); }
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        int ox = box_.x_min + (CW - cols) / 2;   // 居中偏移
        int oy = box_.y_min + (CH - rows) / 2;
        if (mode == "half")
            renderHalf(screen, cols, rows, ox, oy);
        else
            renderQuarter(screen, cols, rows, ox, oy);
    }

    // half-block: 每格 4 宽 x 8 高 像素, 上下两半各一色
    void renderHalf(ftxui::Screen &screen, int cols, int rows, int ox, int oy) {
        int iw = cols * 4, ih = rows * 8;
        int sy = oy;
        for (int y = 0; y < ih; y += 8) {
            int sx = ox;
            for (int x = 0; x < iw; x += 4) {
                int samp[4][8][3];
                for (int by = 0; by < 8; by++)
                    for (int bx = 0; bx < 4; bx++) {
                        int sxp = (x + bx) * img.w / iw;
                        int syp = (y + by) * img.h / ih;
                        if (sxp < 0) sxp = 0;
                        if (sxp >= img.w) sxp = img.w - 1;
                        if (syp < 0) syp = 0;
                        if (syp >= img.h) syp = img.h - 1;
                        const unsigned char *pp =
                            &img.rgb[(size_t)(syp * (long)img.w + sxp) * 3];
                        samp[bx][by][0] = pp[0];
                        samp[bx][by][1] = pp[1];
                        samp[bx][by][2] = pp[2];
                    }
                CharData cd;
                peimg::tiv_find(samp, cd);
                ftxui::Pixel p;
                p.character = peimg::utf8(cd.codepoint);
                p.foreground_color = ftxui::Color((uint8_t)cd.fg[0],
                                                 (uint8_t)cd.fg[1],
                                                 (uint8_t)cd.fg[2]);
                p.background_color = ftxui::Color((uint8_t)cd.bg[0],
                                                 (uint8_t)cd.bg[1],
                                                 (uint8_t)cd.bg[2]);
                if (sx <= box_.x_max && sy <= box_.y_max)
                    screen.PixelAt(sx, sy) = p;
                sx++;
            }
            sy++;
        }
    }

    // quarter-block: 每格 2 宽 x 2 高 像素, 4 个象限, 双色(fg=点亮/bg=未点亮)。
    // 象限位: TL=1, TR=2, BL=4, BR=8; 由 mask 查象限块字符。
    void renderQuarter(ftxui::Screen &screen, int cols, int rows, int ox, int oy) {
        // mask(TL=1,TR=2,BL=4,BR=8) -> 象限块字符码点
        static const unsigned int QCP[16] = {
            0x0020, // 0000 空
            0x2598, // 0001 TL          ▘
            0x259D, // 0010 TR          ▝
            0x2580, // 0011 TL+TR 上半  ▀
            0x2596, // 0100 BL          ▖
            0x258C, // 0101 TL+BL 左半  ▌
            0x259E, // 0110 TR+BL       ▞
            0x259B, // 0111 TL+TR+BL    ▛
            0x2597, // 1000 BR          ▗
            0x259A, // 1001 TL+BR       ▚
            0x2590, // 1010 TR+BR 右半  ▐
            0x259C, // 1011 TL+TR+BR    ▜
            0x2584, // 1100 BL+BR 下半  ▄
            0x2599, // 1101 TL+BL+BR    ▙
            0x259F, // 1110 TR+BL+BR    ▟
            0x2588, // 1111 全          █
        };
        int iw = cols * 2, ih = rows * 2;
        int sy = oy;
        for (int y = 0; y < ih; y += 2) {
            int sx = ox;
            for (int x = 0; x < iw; x += 2) {
                int samp[2][2][3];
                for (int by = 0; by < 2; by++)
                    for (int bx = 0; bx < 2; bx++) {
                        int sxp = (x + bx) * img.w / iw;
                        int syp = (y + by) * img.h / ih;
                        if (sxp < 0) sxp = 0;
                        if (sxp >= img.w) sxp = img.w - 1;
                        if (syp < 0) syp = 0;
                        if (syp >= img.h) syp = img.h - 1;
                        const unsigned char *pp =
                            &img.rgb[(size_t)(syp * (long)img.w + sxp) * 3];
                        samp[bx][by][0] = pp[0];
                        samp[bx][by][1] = pp[1];
                        samp[bx][by][2] = pp[2];
                    }
                // 亮度阈值(单元格平均): 高于阈值的象限置前景, 否则背景
                int lum[2][2];
                int sum = 0;
                for (int by = 0; by < 2; by++)
                    for (int bx = 0; bx < 2; bx++) {
                        lum[bx][by] = (samp[bx][by][0] * 299 +
                                       samp[bx][by][1] * 587 +
                                       samp[bx][by][2] * 114) / 1000;
                        sum += lum[bx][by];
                    }
                int thr = sum / 4;
                int fg[3] = {0, 0, 0}, bg[3] = {0, 0, 0};
                int nfg = 0, nbg = 0;
                unsigned int mask = 0;
                for (int by = 0; by < 2; by++)
                    for (int bx = 0; bx < 2; bx++) {
                        if (lum[bx][by] > thr) {
                            fg[0] += samp[bx][by][0];
                            fg[1] += samp[bx][by][1];
                            fg[2] += samp[bx][by][2];
                            nfg++;
                            // TL=1,TR=2,BL=4,BR=8
                            mask |= (unsigned int)(1 << (by * 2 + bx));
                        } else {
                            bg[0] += samp[bx][by][0];
                            bg[1] += samp[bx][by][1];
                            bg[2] += samp[bx][by][2];
                            nbg++;
                        }
                    }
                if (nfg) { fg[0] /= nfg; fg[1] /= nfg; fg[2] /= nfg; }
                if (nbg) { bg[0] /= nbg; bg[1] /= nbg; bg[2] /= nbg; }
                ftxui::Pixel p;
                p.character = peimg::utf8(QCP[mask & 0xF]);
                p.foreground_color = ftxui::Color((uint8_t)fg[0],
                                                 (uint8_t)fg[1],
                                                 (uint8_t)fg[2]);
                p.background_color = ftxui::Color((uint8_t)bg[0],
                                                 (uint8_t)bg[1],
                                                 (uint8_t)bg[2]);
                if (sx <= box_.x_max && sy <= box_.y_max)
                    screen.PixelAt(sx, sy) = p;
                sx++;
            }
            sy++;
        }
    }
};

// 工厂: 构造一个图片节点元素 (mode: "quarter"(默认) | "half")
static inline ftxui::Element image_element(const std::string &id,
                                           const std::string &mode = "quarter") {
    return std::make_shared<ImageNode>(id, mode);
}
