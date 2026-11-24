#ifndef WEBPQUALITY_H
#define WEBPQUALITY_H

#include <cstddef>

/*
 * 有损 WebP 的质量估计（从码流里读量化索引，不解码像素）。
 *
 * 为什么单独放一个模块：
 *   deduputils.cpp 里有 Qt / OpenCV / libjpeg 一堆依赖，而这段解析只用到
 *   标准库。独立出来之后可以用一个几十行的控制台探针直接编译、拿真实
 *   文件回归，不需要链接整个程序。
 *
 * 返回值和 dedup::estimateImageQuality 的约定一致：
 *   1..100  有损 WebP 的估计质量（与 JPEG 的 IJG 质量分尽量同量纲）
 *   101     无损 WebP（VP8L）
 *   -1      无法估计（不是 WebP / 截断 / 损坏 / 动画的后续帧）
 *
 * data/size 必须是完整文件（要按 RIFF chunk 遍历，VP8 可能在 ICCP/EXIF/ALPH
 * 之后）。只读，不修改任何数据。
 */
namespace webpquality {

/* 与 dedup::kLosslessQuality 必须保持一致（那边是 Qt 侧的常量）。 */
constexpr int kLosslessQuality = 101;

int estimate(const unsigned char *data, size_t size);

} // namespace webpquality

#endif // WEBPQUALITY_H
