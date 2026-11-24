#ifndef DEDUPUTILS_H
#define DEDUPUTILS_H

#include "deduppch.h"
#include <QByteArray>
#include <QImage>
#include <vector>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

// 只为 traceShift 用:不引入 utils.h,免得头文件互相牵连(定义在 utils.cpp)
namespace dedupenv { QString logDir(); }

namespace dedup {

/*
 * Shift 区域选择/切 pair 的现场追踪(写 <logDir>/shift_trace.log)。
 * 只在排查"Shift 从很靠前的位置开始选"这类问题时用,调用点很少、开销可忽略。
 *
 * 写成 inline 是为了让只跑视图单测(lvshift)的构建也能用,不必把整个
 * deduputils.cpp(它带 OpenCV)拖进去。
 */
inline void traceShift(const QString &line)
{
    static const QString path = [] {
        const QString dir = ::dedupenv::logDir();
        return dir.isEmpty() ? QString() : dir + QStringLiteral("/shift_trace.log");
    }();
    if (path.isEmpty()) return;

    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 4 * 1024 * 1024) return;   // 别把内存盘塞满

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    f.write(QStringLiteral("[%1] %2\n").arg(ts, line).toUtf8());
}

/*
 * 计算一段内存(buffer,通常是整个图片文件的内容)的SHA-256原始摘要(32字节)。
 * 结果写入 outRaw32(必须至少有32字节空间)。
 */
void computeSHA256(const QByteArray &data, char *outRaw32);
/* 32字节原始摘要 -> 64个字符的十六进制字符串 */
QString sha256ToHex(const char *raw32);
/* 计算文件的SHA-256并返回“文件内容指纹”;读取失败时返回全0摘要,outOk(可选)表示是否成功 */
QByteArray fileSHA256(const QString &path, bool *outOk = nullptr);
/* 便捷判断:两个32字节摘要是否相同 */
inline bool sameSHA256(const char *a, const char *b) { return std::memcmp(a, b, 32) == 0; }

/* 缩略图统一的最大长宽(px) */
constexpr int kThumbnailMaxSize = 128;

/*
 * 无损格式统一使用的“质量”值(PNG / 无损WebP)。
 * 在评估两个版本哪个更优时,它比任何有损格式都高。
 */
constexpr int kLosslessQuality = 101;

/*
 * 质量无法估计时的中性档位。
 *
 * 目前只会用于有损 WebP 解析失败的情况（文件损坏/截断、动画的后续帧不是
 * keyframe）。正常的有损 WebP 会走 webpquality 的真实估计值，所以不要再把
 * 它当成"有损 WebP 一律 60"的捷径 —— 那个行为已经去掉了。
 */
constexpr int kUnknownQualityRank = 60;

/*
 * 通过文件头判断真实图片格式(比扩展名可靠)。
 * 返回小写格式名:"jpg" / "png" / "webp" / "gif" / "bmp" / "tif",无法判断时返回空字符串。
 */
QString detectImageFormat(const QByteArray &data);

/* 计算质量:jpg用libjpeg量化表估计;png/无损webp返回kLosslessQuality;
 * 有损webp解析VP8帧头的量化索引(见 webpquality.h),其它返回-1(表示无法估计)。 */
int estimateImageQuality(const QByteArray &data, const QString &formatHint = QString());

/*
 * 把任意图像矩阵转换成缩略图:最长边不超过maxSize,保持纵横比。
 * 会正确处理 16bit / 灰度 / BGRA / 不同通道数的输入。
 */
QImage makeThumbnail(const cv::Mat &src, int maxSize = kThumbnailMaxSize);

/*
 * 直接从文件读取并生成缩略图(读取失败返回空QImage)。
 *
 * 实现上优先走 QImageReader 的"按需缩小解码"(JPEG 的 DCT 缩放 / PNG 的行抽样),
 * 避免为了 128px 的缩略图去完整解码一张 1600x2200 的大图 ——
 * 后者在列表里成百上千行滚过去时是实打实的性能灾难。
 * 只有当 Qt 的解码器不支持缩放解码时才退化成"全解码再 resize"。
 */
QImage makeThumbnailFromFile(const QString &path, int maxSize = kThumbnailMaxSize);

uint64_t computePHash(const cv::Mat &img);
uint64_t computeDHash(const cv::Mat &bgr);
int hammingDistance(uint64_t a, uint64_t b);

/* ============================ ORB 几何校验 ============================ */

/*
 * 特征提取参数。改这里必须同时 bump cacheio 的 tscch 版本号,否则旧缓存里的
 * 描述子/关键点与新版判据不匹配(旧记录会被整体丢弃重算)。
 *
 * 为什么缩到长边 1024 再提特征(实测 2673x2732):
 *   ORB(500) 原图  118ms   -> 500 个点撒在 730 万像素上,平均 1.5 万像素才有 1 个
 *   ORB(800) 1024  16ms    -> 快 7 倍,而且特征点在图上分布均匀得多
 * 特征点均匀 >> 特征点多。原图方案是"又慢又稀"。
 */
constexpr int kOrbLongEdge      = 1024;
constexpr int kOrbFeatureCount  = 800;

/*
 * 几何校验的判据。数值来自实测(长边1024/800特征):
 *
 *   样本                    内点   内点/命中
 *   pair1 草稿-成品           88     0.90      <- 必须过
 *   pair2 大图-缩略          334     0.93      <- 必须过
 *   负样本(跨对 4 组)        3-12   0.28-0.67  <- 必须不过
 *   负样本(同页 UI 截图 6 组) 5-46   0.10-0.35  <- 必须不过
 *
 * 两个条件是"与"关系,各挡一类误报:
 *   · minInliers    挡住"只对了十几个点、凑巧排成一条线"的退化解
 *   · minInlierRatio 挡住"命中很多但几何上互相矛盾"的(同页截图 max 46 个内点,
 *                    但一致率只有 0.19)
 */
struct OrbVerifyOptions {
    float  loweRatio      = 0.75f;  // Lowe ratio test 阈值
    int    minGood        = 12;     // 双向互查后的命中数少于此值不做 RANSAC
    int    minInliers     = 30;     // 内点绝对数门槛
    double minInlierRatio = 0.55;   // 内点 / 命中,几何一致率
    double ransacThresh   = 3.0;    // 重投影误差(长边1024坐标下的像素)
    /*
     * 相似变换的尺度容许范围。两张图在提特征前都已把长边缩到 1024,
     * 所以同一张图的两个版本之间尺度应该接近 1;尺度塌到 0 是退化解(必拒)。
     */
    double minScale       = 0.2;
    double maxScale       = 5.0;
};

struct OrbVerifyResult {
    int    keypoints   = 0;    // min(两侧特征点数),用于覆盖率
    int    good        = 0;    // Lowe ratio 通过的命中数
    int    inliers     = 0;    // RANSAC 内点数
    double inlierRatio = 0.0;  // inliers / good,判据之一,也是界面上显示的"ORB similarity"
    double coverage    = 0.0;  // inliers / keypoints,内点覆盖了多少比例的特征点
    bool   matched     = false;
};

/*
 * ORB 几何校验:描述子 + 关键点坐标 -> 是否"同一张图"。
 *
 * 为什么不是单应(cv::findHomography):
 *   单应是 8 自由度,点少的时候容易拟合出荒谬的透视变换,把两张不同的图也"配上"。
 *   而"轻微旋转 + 裁切 + 缩放"本质是一个相似变换(4 自由度:旋转/等比缩放/平移),
 *   用 cv::estimateAffinePartial2D 恰好是欠定程度最低、最不容易过拟合的模型。
 *   实测两者内点数接近(pair1 88 vs 87,pair2 334 vs 339),
 *   但相似变换在负样本上更保守(cross1B2B: 12 个内点 vs 单应的 5 个,
 *   都被"内点≥30"这一条挡住)。
 *
 * ptsA / ptsB 是 Nx2 CV_32F 的关键点坐标(顺序必须与描述子行一一对应)。
 * 没有关键点坐标就做不了几何校验(只剩描述子统计,也就是那个不可靠的老口径),
 * 所以任一为空、或行数与描述子对不上,一律返回 matched=false。
 * 关键点跟描述子一起存进 tscch,正常情况下不会出现这种情况。
 */
OrbVerifyResult verifyOrbPair(const cv::Mat &descA, const cv::Mat &ptsA,
                              const cv::Mat &descB, const cv::Mat &ptsB,
                              const OrbVerifyOptions &opt = OrbVerifyOptions());

cv::Mat loadMat(const QByteArray &bytes);
QByteArray readDataFromPath(const QString &path);
// cv::Mat loadMatFromPath(const QString &path);

// read image with OpenCV, return BGR cv::Mat (or empty if failed)
cv::Mat readImageBGR(const QByteArray &bytes);
cv::Mat readImageBGRA(const QString &path);
// compute pixel-level diff mask and ratios
// returns mask (CV_8U single channel: 0/255), pixelDiffRatio (nonzero pixels / total pixels), diffAreaRatio (sum of connected components area / total pixels)
// std::tuple<cv::Mat, double, double> computePixelDiff(const cv::Mat &aBGR, const cv::Mat &bBGR);
cv::Mat ensureBGR8(const cv::Mat &src);

/*
 * 返回: tuple(grayDiff, pixelDiffRatio, meanDiff)
 * - grayDiff: CV_8UC1, per-pixel absdiff magnitude (0..255)
 * - pixelDiffRatio: double (0..1) = fraction of pixels with grayDiff > thresh
 * - meanDiff: double (0..1) = mean(grayDiff)/255.0
 */
std::tuple<cv::Mat, double, double> computePixelDiff(const cv::Mat &aBGR_in, const cv::Mat &bBGR_in, double threshVal = 4.0);

/*
 * 把一对图片对齐到同一张画布上(像素比较和显示都用它)。
 *
 * 名字刻意不叫 makeAlignedPair:datasetwindow.cpp 里已经有一个同名的**文件内**
 * 局部函数,一旦同名,那边未限定的调用会变成二义(实测编译报 C2668)。
 *
 * 规则(按需求):
 *   1) 画布 = 两张图各方向取最大(union),两边的输出尺寸一定相同;
 *   2) 缩放系数 = min(画布宽/本图宽, 画布高/本图高),因为画布取的是 max,
 *      所以系数恒 >= 1 —— **只放大,绝不降采样**(降采样会让两张不同的图
 *      在像素比较里变得一样,进而误删真实数据);
 *   3) 具体到一张图:
 *        - 两条边都比对方小 -> 放大到"其中一条边先够到对方",另一条边补边;
 *        - 一条边已经更大、另一条更小 -> 系数正好是 1,**完全不缩放**,只补边;
 *      (旧实现在"系数≈1"时跳过补边,再由调用方 resize 到同一尺寸,
 *       于是这种图会被硬拉变形 —— 长宽比失真的根源)
 *   4) 补边用黑色、左右/上下各分一半 -> 内容在画布里居中,两张图的中心自然对齐。
 *
 * 输出一定是【独立缓冲区】,调用方可以就地改写,不会污染缓存里的原图。
 */
void alignPairToCommonSize(const cv::Mat &A, const cv::Mat &B, cv::Mat &outA, cv::Mat &outB);

QImage cvMatToQImageRGBA(const cv::Mat &src); // for overlay (RGBA)
QImage cvMatToQImageRGB(const cv::Mat &src);

/*
 * 把矩阵转成 QImage::Format_RGB32(不透明 32bpp)。
 *
 * 专门给"去重窗口里要反复显示的大图"用。原因(实测 12.6MP):
 *   QPixmap::fromImage(RGB888 / RGBA8888) ≈ 24~29ms —— 每次都要做一遍全图格式转换;
 *   QPixmap::fromImage(RGB32)             ≈ 0ms   —— 无需转换,QImage 直接就能被 pixmap 接过去。
 * 去重窗口每切一对图都要走两次这个转换,而且 highlight 定时器还会不停重绘,
 * 所以这里选择在后台线程多花一次 cvtColor(约 10ms),换 UI 线程每次省 24ms。
 *
 * 注意:RGB32 没有 alpha 通道,4 通道输入的 alpha 会被丢弃。
 * 需要保留透明度的场景请用 cvMatToDisplayQImage()。
 */
QImage cvMatToQImageRGB32(const cv::Mat &src);

/*
 * 显示图专用转换:先在后台判断这张图"是不是真的有透明像素",再决定格式。
 *   完全不透明(绝大多数情况,包括那些"有 alpha 通道但全 255"的 PNG)
 *       -> Format_RGB32,后续 QPixmap::fromImage 和绘制都是零转换/最快路径;
 *   真的存在 alpha != 255 的像素
 *       -> Format_RGBA8888,交给显示控件走棋盘格,保留原来的观感。
 * 判断本身(抽一遍 alpha 通道取最小值)只在后台跑一次,不占 UI 线程。
 */
QImage cvMatToDisplayQImage(const cv::Mat &src);

QStringList supportedExtensions(); // list of supported image extensions

/*
 * path 是否等于 dir、或者位于 dir 之下(忽略大小写,Windows 友好)。
 * 用于判断一个文件归哪个 tscch / 哪个 input folder 管。
 */
bool isUnderOrEqual(const QString &path, const QString &dir);

/*
 * 该文件相对于 rootFolder 的"顶层子目录"名。
 *   rootFolder/x.jpg        -> ""      (直接放在 rootFolder 下)
 *   rootFolder/a/x.jpg      -> "a"
 *   rootFolder/a/b/x.jpg    -> "a"
 * internal == 1(半勾选)时用它判断两个文件是不是在同一个顶层子目录里。
 */
QString topLevelSubDirOf(const QString &rootFolder, const QString &filePath);



struct JpegQualityEstimate {
    int quality = -1;              // 1..100, 或 -1 表示无法可靠估计
    double error = std::numeric_limits<double>::infinity(); // 越小越像标准 IJG 表
    double confidence = 0.0;       // 0..1
    bool likelyStandardIJG = false;
};
JpegQualityEstimate estimateJpegQuality(jpeg_decompress_struct* cinfo);



}


#endif // DEDUPUTILS_H
