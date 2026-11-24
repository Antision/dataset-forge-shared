#include "webpquality.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

using uchar = unsigned char;

constexpr int kLosslessQuality = webpquality::kLosslessQuality;
constexpr int kNotFound = -2;   // 内部用:没找到图像数据(顶层再映射成 -1)

uint32_t readLE32(const uchar *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/*
 * RFC 6386 §7.3 的 bool 解码器。
 *
 * 这一段是整件事的关键：VP8 的帧头（包括量化索引）不是按字节对齐的，而是
 * 用算术编码的 bit 流写的。以前那段代码直接读 q[9]，读到的其实是帧头里
 * "高度"的高字节，所以不管图片质量好坏都只会得到 0/100 这种假值。
 *
 * 实现严格照 RFC 的参考解码器：
 *   init: value = (buf[0] << 8) | buf[1]; range = 255; bit_count = 0
 *   read_bool: split = 1 + ((range - 1) * prob >> 8)
 *              value >= split << 8 ? 1 : 0
 *              然后 while (range < 128) 左移并每 8 位补一个字节。
 *
 * 越界时把 ok 置 false 并继续返回 0，让调用方走完语法后统一判失败，
 * 避免在半个字段上直接返回导致后面越读越远。
 */
class BoolDecoder
{
public:
    BoolDecoder(const uchar *data, int size)
        : m_data(data), m_size(size), m_pos(2),
          m_value(0), m_range(255), m_bitCount(0), m_ok(size >= 2)
    {
        if (m_ok)
            m_value = (static_cast<uint32_t>(data[0]) << 8) | data[1];
    }

    bool ok() const { return m_ok; }

    int readBool(int probability)
    {
        if (!m_ok) return 0;

        const uint32_t split = 1 + (((m_range - 1) * static_cast<uint32_t>(probability)) >> 8);
        const uint32_t bigSplit = split << 8;

        int bit = 0;
        if (m_value >= bigSplit) {
            bit = 1;
            m_range -= split;
            m_value -= bigSplit;
        } else {
            m_range = split;
        }

        while (m_range < 128) {
            m_value <<= 1;
            m_range <<= 1;
            if (++m_bitCount == 8) {
                m_bitCount = 0;
                if (m_pos < m_size)
                    m_value |= m_data[m_pos];
                else
                    m_ok = false;   // 截断:后面补 0,但整次解析判失败
                ++m_pos;
            }
        }
        return bit;
    }

    uint32_t readLiteral(int bits)
    {
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i)
            v = (v << 1) | static_cast<uint32_t>(readBool(128));
        return v;
    }

    /* 有符号字面量:先幅度(高位在前),再 1 位符号(0 正 / 1 负)。 */
    int readSigned(int magnitudeBits)
    {
        const int magnitude = static_cast<int>(readLiteral(magnitudeBits));
        const int negative = readBool(128);
        return negative ? -magnitude : magnitude;
    }

private:
    const uchar *m_data;
    int m_size;
    int m_pos;
    uint32_t m_value;
    uint32_t m_range;
    int m_bitCount;
    bool m_ok;
};

struct Vp8QuantInfo
{
    int yacQi = 0;                  // VP8 帧头里的基准 Y-AC 量化索引(0..127)
    bool hasSegmentData = false;    // 是否真的带了 segment 量化特征
    bool absoluteSegments = false;  // true=绝对值,false=相对 yacQi 的增量
    bool hasValue[4] = { false, false, false, false };
    int segmentQi[4] = { 0, 0, 0, 0 };
};

/*
 * 解析 keyframe 的压缩帧头，一直读到 §9.6 的 yac_qi。
 *
 * 字段顺序(RFC 6386 §9.2 ~ §9.6)：
 *   color_space L(1), clamp_type L(1)              [仅 keyframe]
 *   segmentation_enabled L(1)
 *     若开启: update_mb_segmentation_map L(1), update_segment_feature_data L(1)
 *       若更新特征: segment_feature_mode L(1), 4 × [flag L(1) + magnitude L(7) + sign L(1)],
 *                   4 × [flag L(1) + magnitude L(6) + sign L(1)]   (后 4 个是 loop filter)
 *       若更新 map: 3 × [flag L(1) + prob L(8)]
 *   filter_type L(1), loop_filter_level L(6), sharpness_level L(3)
 *   loop_filter_adj_enable L(1)
 *     若开启: mode_ref_lf_delta_update L(1)
 *       若更新: 4 × [flag L(1) + magnitude L(6) + sign L(1)]
 *               4 × [flag L(1) + magnitude L(6) + sign L(1)]
 *   nbr_dct_partitions L(2)
 *   yac_qi L(7)
 *
 * segment_feature_mode 的取值以 RFC 6386 §19.2 / erratum 7519 为准：
 *   0 = delta(相对 yac_qi)，1 = absolute。
 *   §9.3 的正文把这两个值写反了，erratum 明确指出 §19.2 才是对的。
 *   实测也验证了这一点：一张 quality≈90 的图读到 mode=1、段值 {12,11,9,7}，
 *   按绝对值解释与编码设定吻合；按增量解释会变成 24/23/21/19，明显过低。
 */
bool parseVp8QuantInfo(const uchar *p, int n, Vp8QuantInfo &info)
{
    if (n < 10) return false;
    if (p[0] & 0x01) return false;                                  // interframe
    if (!(p[3] == 0x9d && p[4] == 0x01 && p[5] == 0x2a)) return false; // keyframe start code

    BoolDecoder br(p + 10, n - 10);

    br.readLiteral(1);   // color_space
    br.readLiteral(1);   // clamp_type

    const bool segmentationEnabled = br.readLiteral(1) != 0;
    if (segmentationEnabled) {
        const bool updateMap = br.readLiteral(1) != 0;
        const bool updateFeatureData = br.readLiteral(1) != 0;

        if (updateFeatureData) {
            info.hasSegmentData = true;
            info.absoluteSegments = br.readLiteral(1) != 0;

            for (int i = 0; i < 4; ++i) {
                if (br.readLiteral(1)) {
                    info.segmentQi[i] = br.readSigned(7);
                    info.hasValue[i] = true;
                }
            }
            // 循环滤波的 4 个段特征：与质量无关，但必须读掉，否则后面全错位
            for (int i = 0; i < 4; ++i) {
                if (br.readLiteral(1))
                    br.readSigned(6);
            }
        }

        if (updateMap) {
            for (int i = 0; i < 3; ++i) {
                if (br.readLiteral(1))
                    br.readLiteral(8);
            }
        }
    }

    br.readLiteral(1);   // filter_type
    br.readLiteral(6);   // loop_filter_level
    br.readLiteral(3);   // sharpness_level

    if (br.readLiteral(1)) {          // loop_filter_adj_enable
        if (br.readLiteral(1)) {      // mode_ref_lf_delta_update
            for (int i = 0; i < 4; ++i)
                if (br.readLiteral(1)) br.readSigned(6);
            for (int i = 0; i < 4; ++i)
                if (br.readLiteral(1)) br.readSigned(6);
        }
    }

    br.readLiteral(2);   // nbr_dct_partitions
    info.yacQi = static_cast<int>(br.readLiteral(7));

    return br.ok();
}

/* 4 个段的量化索引取平均。没有段数据时就是 yac_qi 本身。 */
double effectiveQuantIndex(const Vp8QuantInfo &info)
{
    if (!info.hasSegmentData)
        return static_cast<double>(info.yacQi);

    double sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        int q = info.yacQi;
        if (info.hasValue[i]) {
            q = info.absoluteSegments ? info.segmentQi[i]
                                      : info.yacQi + info.segmentQi[i];
        }
        sum += std::clamp(q, 0, 127);
    }
    return sum / 4.0;
}

/*
 * 量化索引 -> 质量分。
 *
 * 标定方法(不是拍脑袋)：用 libwebp 默认参数(segments=4, sns-strength=50)
 * 对同一张 1412x1256 的图分别编码 quality=5..95，然后用本文件同一个解析器
 * 从码流里读出"段平均 yac_qi"，得到：
 *
 *   quality :  95   90   85   80   75   70   60   50   40   30   20   10    5
 *   q(avg)  : 4.5  9.8 16.0 22.2 29.8 32.5 37.8 44.0 50.2 58.2 68.2 81.8 92.0
 *
 * 这里把表反过来用：给一个 q，线性插值出 quality。
 *
 * 为什么不用 libwebp 的 quality->q 公式直接反解：那个公式里还有
 * sns_strength 和每个 segment 的 alpha(内容相关，码流里没有)，固定假设
 * 一个 alpha 会系统性偏低(实测 q=44 会被算成 29)。用编码器自己的标定曲线，
 * 至少和"大多数人用默认参数压出来的 webp"是对齐的；典型误差约 ±5 分。
 */
int qualityFromQuantIndex(double q)
{
    static const double kQ[] = {
        0.0, 4.5, 9.8, 16.0, 22.2, 29.8, 32.5, 37.8,
        44.0, 50.2, 58.2, 68.2, 81.8, 92.0, 127.0
    };
    static const double kQuality[] = {
        100.0, 95.0, 90.0, 85.0, 80.0, 75.0, 70.0, 60.0,
        50.0, 40.0, 30.0, 20.0, 10.0, 5.0, 1.0
    };
    constexpr int kCount = static_cast<int>(sizeof(kQ) / sizeof(kQ[0]));

    if (q <= kQ[0]) return 100;
    if (q >= kQ[kCount - 1]) return 1;

    for (int i = 1; i < kCount; ++i) {
        if (q <= kQ[i]) {
            const double t = (q - kQ[i - 1]) / (kQ[i] - kQ[i - 1]);
            const double v = kQuality[i - 1] + t * (kQuality[i] - kQuality[i - 1]);
            return std::clamp(static_cast<int>(std::lround(v)), 1, 100);
        }
    }
    return 1;
}

/*
 * 遍历一层 RIFF chunk，找到 VP8 / VP8L 就返回。
 * depth 只用于 ANMF(动画帧)：ANMF 的 payload 前 16 字节是帧头，后面才是
 * 真正的 VP8/VP8L 子 chunk，递归一层即可。
 */

int scanChunks(const uchar *p, int n, int depth)
{
    int offset = 0;
    while (offset + 8 <= n) {
        const uchar *fourcc = p + offset;
        const uint32_t chunkSize = readLE32(p + offset + 4);
        const int payload = offset + 8;
        const int avail = n - payload;
        if (avail < 0) break;

        if (std::memcmp(fourcc, "VP8L", 4) == 0)
            return kLosslessQuality;

        if (std::memcmp(fourcc, "VP8 ", 4) == 0) {
            const int bytes = static_cast<int>(
                std::min<uint32_t>(chunkSize, static_cast<uint32_t>(avail)));
            Vp8QuantInfo info;
            if (!parseVp8QuantInfo(p + payload, bytes, info))
                return -1;
            return qualityFromQuantIndex(effectiveQuantIndex(info));
        }

        if (depth == 0 && std::memcmp(fourcc, "ANMF", 4) == 0
            && chunkSize > 16 && avail > 16) {
            const uint32_t subSize = chunkSize - 16;
            const int subAvail = avail - 16;
            const int bytes = static_cast<int>(
                std::min<uint32_t>(subSize, static_cast<uint32_t>(subAvail)));
            const int r = scanChunks(p + payload + 16, bytes, depth + 1);
            if (r != kNotFound)
                return r;   // 第一帧就确定结果(静态图/动画首帧都是 keyframe)
        }

        // chunk 按偶数字节对齐；advance 用 64 位算，避免恶意 chunkSize 溢出
        const uint64_t advance = static_cast<uint64_t>(chunkSize) + (chunkSize & 1u);
        if (advance == 0 || advance > static_cast<uint64_t>(avail))
            break;
        offset = payload + static_cast<int>(advance);
    }
    return kNotFound;
}

} // namespace

int webpquality::estimate(const unsigned char *data, size_t size)
{
    if (!data || size < 12)
        return -1;
    if (std::memcmp(data, "RIFF", 4) != 0)
        return -1;
    if (std::memcmp(data + 8, "WEBP", 4) != 0)
        return -1;

    const size_t bodySize = size - 12;
    const int n = static_cast<int>(std::min<size_t>(bodySize, static_cast<size_t>(INT_MAX)));

    const int r = scanChunks(data + 12, n, 0);
    return (r == kNotFound) ? -1 : r;
}
