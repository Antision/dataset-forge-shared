#include "deduputils.h"
#include "dedupenv.h"
#include "webpquality.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <climits>
#if defined(_MSC_VER)
#  include <nmmintrin.h>   // _mm_popcnt_u64:verifyOrbPair 的手写 Hamming 用
#  define DEDUP_OPTIMIZE_ON()  __pragma(optimize("", on))
#  define DEDUP_OPTIMIZE_OFF() __pragma(optimize("", off))
#else
#  define DEDUP_OPTIMIZE_ON()
#  define DEDUP_OPTIMIZE_OFF()
#endif

using namespace dedup;

void dedup::computeSHA256(const QByteArray &data, char *outRaw32)
{
    if (!outRaw32) return;
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!data.isEmpty())
        h.addData(data);
    const QByteArray digest = h.result(); // 32字节
    std::memset(outRaw32, 0, 32);
    std::memcpy(outRaw32, digest.constData(),
                static_cast<size_t>(std::min<int>(32, digest.size())));
}

QString dedup::sha256ToHex(const char *raw32)
{
    if (!raw32) return QString();
    return QString::fromLatin1(QByteArray(raw32, 32).toHex());
}

QByteArray dedup::fileSHA256(const QString &path, bool *outOk)
{
    if (outOk) *outOk = false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray(32, '\0');

    QCryptographicHash h(QCryptographicHash::Sha256);
    bool readOk = true;
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(1 << 20); // 1MB
        if (chunk.isEmpty()) {
            if (f.error() != QFileDevice::NoError) readOk = false;
            break;
        }
        h.addData(chunk);
    }
    f.close();

    const QByteArray digest = h.result();
    if (digest.size() != 32 || !readOk)
        return QByteArray(32, '\0');

    if (outOk) *outOk = true;
    return digest;
}

QImage dedup::makeThumbnail(const cv::Mat &src, int maxSize)
{
    if (src.empty() || maxSize <= 0) return QImage();

    cv::Mat bgr = ensureBGR8(src);
    if (bgr.empty()) return QImage();

    const int w = bgr.cols;
    const int h = bgr.rows;
    if (w <= 0 || h <= 0) return QImage();

    const double scale = std::min(1.0,
        static_cast<double>(maxSize) / static_cast<double>(std::max(w, h)));
    const int tw = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int th = std::max(1, static_cast<int>(std::lround(h * scale)));

    cv::Mat resized;
    if (tw == w && th == h)
        resized = bgr;
    else
        cv::resize(bgr, resized, cv::Size(tw, th), 0, 0,
                   scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);

    return cvMatToQImageRGB(resized);
}

QImage dedup::makeThumbnailFromFile(const QString &path, int maxSize)
{
    if (path.isEmpty() || maxSize <= 0) return QImage();

    /*
     * 优先走 QImageReader 的"缩放解码"。
     *
     * 列表懒加载会一口气为几十行各生成 2 张缩略图。如果每张都先把
     * 1600x2200 的大图完整解码成 BGR(约 10MP / 30MB)再缩到 128,
     * 光是内存带宽和越界访问就够把 UI 卡死。
     *
     * QImageReader::setScaledSize 会让底层解码器在解码阶段就直接输出目标尺寸:
     *   - JPEG(经 libjpeg):走 DCT 1/2,1/4,1/8 缩放,只算需要的系数;
     *   - PNG:逐行抽样;
     * 不能缩放解码的格式(某些 TIFF/BMP 插件)则退化为全解码,行为与原来一致。
     */
    {
        QImageReader reader(path);
        reader.setAutoTransform(true);   // 尊重 EXIF 方向,和"打开图片"看到的一致
        if (reader.canRead()) {
            const QSize src = reader.size();
            if (src.isValid() && src.width() > 0 && src.height() > 0) {
                // 大图用"至少缩到这么多"来算,解码器只会给不小于这个尺寸的结果,
                // 然后再用 OpenCV 做最后一步到精确尺寸的缩放(质量更稳)。
                const double scale = std::min(1.0,
                    static_cast<double>(maxSize) / static_cast<double>(std::max(src.width(), src.height())));
                const int tw = std::max(1, static_cast<int>(std::lround(src.width()  * scale)));
                const int th = std::max(1, static_cast<int>(std::lround(src.height() * scale)));

                /*
                 * 只求"缩小解码"这一个好处:目标尺寸比原图小才设置,
                 * 原图本来就 ≤ maxSize 的话直接全读(反正很小)。
                 */
                if (scale < 1.0)
                    reader.setScaledSize(QSize(tw, th));

                const QImage img = reader.read();
                if (!img.isNull()) {
                    if (img.width() <= maxSize && img.height() <= maxSize)
                        return img;
                    // 缩放解码给的是"不小于"目标尺寸的结果,再精确缩一次
                    return img.scaled(maxSize, maxSize, Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
                }
            }
        }
    }

    // 兜底:交给原来的 OpenCV 全解码路径(部分插件不支持缩放解码)
    const QByteArray data = readDataFromPath(path);
    if (data.isEmpty()) return QImage();
    try {
        return makeThumbnail(loadMat(data), maxSize);
    } catch (...) {
        return QImage();
    }
}

/* ==================== 图片格式识别 & 质量估计 ==================== */

QString dedup::detectImageFormat(const QByteArray &data)
{
    if (data.size() < 12) return QString();

    const uchar *p = reinterpret_cast<const uchar*>(data.constData());
    const int n = data.size();

    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return QStringLiteral("jpg");
    if (p[0] == 0x89 && data.mid(1, 3) == "PNG")     return QStringLiteral("png");
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F')   return QStringLiteral("gif");
    if (p[0] == 'B' && p[1] == 'M')                  return QStringLiteral("bmp");
    if ((p[0] == 'I' && p[1] == 'I' && p[2] == 0x2A) ||
        (p[0] == 'M' && p[1] == 'M' && p[2] == 0x00)) return QStringLiteral("tif");
    if (n >= 12 && data.mid(0, 4) == "RIFF" && data.mid(8, 4) == "WEBP")
        return QStringLiteral("webp");

    return QString();
}

/*
 * WebP 的质量估计在 webpquality.cpp 里（RFC 6386 的 bool 解码器 + VP8 量化
 * 索引），那边不依赖 Qt/OpenCV，方便用独立探针拿真实文件回归。
 *
 * 旧实现直接读帧头第 10 个字节当 quant index，实际读到的是"高度"的高字节；
 * 而且把关键帧判断写反了（RFC 6386: bit0=0 才是 keyframe），所以所有有损
 * WebP 都返回 -1，最后由 qualityRank 兜底成 60，看上去像"算出来的 60"。
 */

/* 两处的"无损"常量必须一致，改一处忘了另一处会在编译期直接报出来。 */
static_assert(kLosslessQuality == webpquality::kLosslessQuality,
              "dedup::kLosslessQuality 与 webpquality::kLosslessQuality 必须一致");

int dedup::estimateImageQuality(const QByteArray &data, const QString &formatHint)
{
    if (data.isEmpty()) return -1;

    QString fmt = detectImageFormat(data);
    if (fmt.isEmpty()) {
        // 退回到扩展名(有些格式头比较特殊,例如 jpe)
        fmt = formatHint.toLower();
        if (fmt.startsWith(QLatin1String("jp")) &&
            (fmt.endsWith(QLatin1Char('g')) || fmt.endsWith(QLatin1Char('e'))))
            fmt = QStringLiteral("jpg");
    }

    if (fmt == QLatin1String("jpg")) {
        try {
            jpeg_decompress_struct cinfo{};
            jpeg_error_mgr jerr{};
            cinfo.err = jpeg_std_error(&jerr);
            jpeg_create_decompress(&cinfo);
            jpeg_mem_src(&cinfo,
                         reinterpret_cast<const unsigned char*>(data.constData()),
                         static_cast<unsigned long>(data.size()));
            jpeg_read_header(&cinfo, TRUE);
            const int q = estimateJpegQuality(&cinfo).quality;
            jpeg_destroy_decompress(&cinfo);
            return q;
        } catch (...) {
            return -1;
        }
    }

    if (fmt == QLatin1String("png"))
        return kLosslessQuality;   // PNG为无损格式

    if (fmt == QLatin1String("webp"))
        return webpquality::estimate(reinterpret_cast<const uchar*>(data.constData()),
                                     static_cast<size_t>(data.size())); // 无损->101,有损->估计值

    return -1;
}

uint64_t dedup::computePHash(const cv::Mat &img) {
    if (img.empty()) return 0;
    cv::Mat gray;
    if (img.channels() == 3) cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    else gray = img.clone();
    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(32,32), 0,0, cv::INTER_AREA);
    resized.convertTo(resized, CV_32F);
    cv::Mat dctmat;
    cv::dct(resized, dctmat);
    std::vector<float> vals;
    for(int r=0;r<8;++r) for(int c=0;c<8;++c) vals.push_back(dctmat.at<float>(r,c));
    std::vector<float> v2 = vals;
    std::nth_element(v2.begin(), v2.begin() + v2.size()/2, v2.end());
    float median = v2[v2.size()/2];
    uint64_t hash = 0;
    for(size_t i=0;i<vals.size();++i) if (vals[i] > median) hash |= (uint64_t(1) << i);
    return hash;
}

namespace {

/*
 * kNN(k=2)的结果:最近邻下标、最近距离、次近距离。
 * verifyOrbPair 只用到这三样东西(ratio test + 双向互查)。
 */
struct KnnHit {
    int bestIdx    = -1;
    int bestDist   = INT_MAX;
    int secondDist = INT_MAX;
};

/*
 * 手写 Hamming kNN,替代 cv::BFMatcher::knnMatch(..., 2)。
 *
 * 为什么要自己写(实测,800x800 描述子,本机 Debug 构建):
 *     cv::BFMatcher::knnMatch      12~13 ms
 *     这段 popcnt 双循环           1.5~2 ms
 * 建对阶段每对候选图要跑两遍,几万对就是几分钟的差距 —— 这就是"扫完卡很久"的大头。
 * BFMatcher 慢不在算法,而在:通用 distance 分发、每次查询要维护/排序 k 个候选、
 * 以及内部再套一层 parallel_for_(800 个查询拆给线程池,单次调用的调度开销比计算还大)。
 * 而这里的数据量只有 800x32B x 2 = 51KB,整个塞在 L2 里,双重循环最快。
 *
 * 语义必须和 BFMatcher 一致:距离相同取下标小的那个(best 用严格小于,
 * second 用 严格小于 secondDist),这样结果与老实现逐位相同(已用 30 对真实
 * 描述子对比验证过首近邻完全一致)。
 *
 * 注意:本工程日常跑的是 Debug(/Od),所以这里显式打开优化 ——
 * 纯数值循环,没有副作用,开优化不改变语义。
 */
DEDUP_OPTIMIZE_ON()
static inline int hammingWords(const quint64 *a, const quint64 *b, int words)
{
    int d = 0;
    for (int k = 0; k < words; ++k)
        d += int(_mm_popcnt_u64(a[k] ^ b[k]));
    return d;
}

static bool knn2Hamming(const cv::Mat &A, const cv::Mat &B, std::vector<KnnHit> &out)
{
    if (A.empty() || B.empty()) return false;
    if (A.type() != CV_8U || B.type() != CV_8U) return false;
    if (A.cols != B.cols || A.cols % 8 != 0) return false;
    if (!A.isContinuous() || !B.isContinuous()) return false;
    if (A.rows < 2 || B.rows < 2) return false;

    const int na = A.rows, nb = B.rows, words = A.cols / 8;
    out.assign(size_t(na), KnnHit());

    const quint64 *baseA = reinterpret_cast<const quint64 *>(A.data);
    const quint64 *baseB = reinterpret_cast<const quint64 *>(B.data);

    for (int i = 0; i < na; ++i) {
        const quint64 *a = baseA + size_t(i) * size_t(words);
        int d1 = INT_MAX, d2 = INT_MAX, j1 = -1;
        for (int j = 0; j < nb; ++j) {
            const int d = hammingWords(a, baseB + size_t(j) * size_t(words), words);
            if (d < d1) { d2 = d1; d1 = d; j1 = j; }
            else if (d < d2) { d2 = d; }
        }
        out[size_t(i)].bestIdx    = j1;
        out[size_t(i)].bestDist   = d1;
        out[size_t(i)].secondDist = d2;
    }
    return true;
}
DEDUP_OPTIMIZE_OFF()

/* cv::BFMatcher 兜底(描述子不是 8 字节整数倍 / 内存不连续时才走这里) */
static void knn2Bf(const cv::Mat &A, const cv::Mat &B, std::vector<KnnHit> &out)
{
    cv::BFMatcher matcher(cv::NORM_HAMMING, /*crossCheck=*/false);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(A, B, knn, 2);

    out.assign(size_t(A.rows), KnnHit());
    for (int i = 0; i < int(knn.size()) && i < A.rows; ++i) {
        const auto &v = knn[size_t(i)];
        if (v.empty()) continue;
        out[size_t(i)].bestIdx  = v[0].trainIdx;
        out[size_t(i)].bestDist = int(v[0].distance);
        if (v.size() >= 2) out[size_t(i)].secondDist = int(v[1].distance);
    }
}

/*
 * 一个方向的 kNN + Lowe ratio test。
 * 返回"通过 ratio 的命中数";out 供调用方做双向互查。
 */
static int knn2AndRatio(const cv::Mat &A, const cv::Mat &B, double ratio,
                        std::vector<KnnHit> &out)
{
    if (!knn2Hamming(A, B, out))
        knn2Bf(A, B, out);

    int pass = 0;
    for (const KnnHit &h : out) {
        if (h.secondDist == INT_MAX || h.bestDist == INT_MAX) continue;
        if (double(h.bestDist) < ratio * double(h.secondDist)) ++pass;
    }
    return pass;
}

} // namespace

dedup::OrbVerifyResult dedup::verifyOrbPair(const cv::Mat &descA, const cv::Mat &ptsA,
                                            const cv::Mat &descB, const cv::Mat &ptsB,
                                            const OrbVerifyOptions &opt)
{
    OrbVerifyResult r;

    if (descA.empty() || descB.empty()) return r;
    if (descA.type() != CV_8U || descB.type() != CV_8U) return r;
    if (descA.cols != descB.cols) return r;

    /*
     * 几何校验必须有坐标。没有坐标(或者行数对不上,例如缓存被截断)时
     * 直接返回"不匹配" —— 宁可漏,也不能退回到"只看描述子统计"那个
     * 会把草稿/成品压到 0.1、把无关图抬到 0.09 的不可靠口径。
     */
    if (ptsA.empty() || ptsB.empty()) return r;
    if (ptsA.type() != CV_32F || ptsB.type() != CV_32F) return r;
    if (ptsA.cols < 2 || ptsB.cols < 2) return r;
    if (ptsA.rows != descA.rows || ptsB.rows != descB.rows) return r;

    r.keypoints = std::min(descA.rows, descB.rows);
    if (r.keypoints <= 0) return r;

    try {
        /*
         * 先做一遍单向筛选:如果 A->B 里通过 ratio test 的都不到 minGood,
         * 反向那一遍(最贵的操作)就不用算了,绝大多数候选对在这里被挡掉。
         */
        std::vector<KnnHit> ab;
        const int forwardPass = knn2AndRatio(descA, descB, double(opt.loweRatio), ab);
        if (forwardPass < opt.minGood) return r;

        std::vector<KnnHit> ba;
        knn2AndRatio(descB, descA, double(opt.loweRatio), ba);

        /*
         * ===== 必须双向互查,单向 knnMatch 会要命 =====
         *
         * 只用 A->B 的 kNN 时,没有"一个 B 点只能被一个 A 点认领"的约束:
         * A 里几十个描述子可以同时把 B 的同一个描述子当成最佳匹配。
         * 于是 estimateAffinePartial2D 会给出一个【尺度≈0】的退化解 ——
         * 把 A 的一整片区域压成 B 的同一个点,重投影误差当然极小,
         * 内点数一路飙到几十个。
         *
         * 实测:一张 390x844 的手机界面截图和一张 2131x3409 的插画,
         * 这样能"匹配"出 55 命中 / 53 内点 / 一致率 0.96,而 B 侧内点包围盒是 0x0
         * (全部落在同一个点上)。这是纯粹的假阳性。
         *
         * 双向都过 ratio test、且互为最佳,才留下。
         */
        std::vector<cv::Point2f> src, dst;
        src.reserve(size_t(descA.rows));
        dst.reserve(size_t(descA.rows));
        for (int ia = 0; ia < descA.rows && ia < int(ab.size()); ++ia) {
            const KnnHit &h = ab[size_t(ia)];
            if (h.bestIdx < 0 || h.secondDist == INT_MAX) continue;
            if (!(double(h.bestDist) < double(opt.loweRatio) * double(h.secondDist))) continue;

            const int ib = h.bestIdx;
            if (ib < 0 || ib >= int(ba.size())) continue;
            const KnnHit &hb = ba[size_t(ib)];
            if (hb.bestIdx != ia) continue;              // 不是互为最佳 -> 丢掉
            if (hb.secondDist == INT_MAX) continue;
            if (!(double(hb.bestDist) < double(opt.loweRatio) * double(hb.secondDist))) continue;

            src.push_back(ptsA.at<cv::Vec2f>(ia, 0));
            dst.push_back(ptsB.at<cv::Vec2f>(ib, 0));
        }
        r.good = static_cast<int>(src.size());
        if (r.good < opt.minGood) return r;

        /*
         * 相似变换(旋转 + 等比缩放 + 平移)而不是单应:
         * 4 个自由度,点少的时候不会像 8 自由度的单应那样拟合出荒谬透视,
         * 把"碰巧有点对应"的两张图也凑成一张。
         */
        cv::Mat inlierMask;
        const cv::Mat M = cv::estimateAffinePartial2D(src, dst, inlierMask,
                                                      cv::RANSAC, opt.ransacThresh,
                                                      500, 0.99, 10);
        if (M.empty() || inlierMask.empty()) return r;

        /*
         * 尺度合理性校验:两张图在提取前都已经把长边缩到 1024 以内,
         * 所以同一张图的两个版本之间,相似变换的尺度应该接近 1。
         * 尺度塌成 0(上面说的退化解)或者大到离谱,一律不认。
         */
        const double scale = std::sqrt(double(M.at<double>(0, 0)) * M.at<double>(0, 0)
                                       + double(M.at<double>(1, 0)) * M.at<double>(1, 0));
        if (!(scale >= opt.minScale && scale <= opt.maxScale)) return r;

        r.inliers     = cv::countNonZero(inlierMask);
        r.inlierRatio = static_cast<double>(r.inliers) / static_cast<double>(r.good);
        r.coverage    = static_cast<double>(r.inliers) / static_cast<double>(r.keypoints);
        r.matched     = (r.inliers >= opt.minInliers)
                        && (r.inlierRatio >= opt.minInlierRatio);
    } catch (...) {
        // OpenCV 内部异常(内存/极端输入)当成"不匹配",绝不让它冒出去打断扫描
        r.inliers     = 0;
        r.inlierRatio = 0.0;
        r.coverage    = 0.0;
        r.matched     = false;
    }

    return r;
}
uint64_t dedup::computeDHash(const cv::Mat &bgr)
{
    if (bgr.empty())
        return 0;

    cv::Mat gray;
    if (bgr.channels() == 3) {
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = bgr;
    }

    // resize to 9x8
    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(9, 8), 0, 0, cv::INTER_LINEAR);

    uint64_t hash = 0;
    int bit = 63;

    for (int y = 0; y < 8; ++y) {
        const uchar* row = resized.ptr<uchar>(y);
        for (int x = 0; x < 8; ++x) {
            if (row[x] > row[x + 1]) {
                hash |= (uint64_t(1) << bit);
            }
            bit--;
        }
    }

    return hash;
}

int dedup::hammingDistance(uint64_t a, uint64_t b) {
#ifdef _MSC_VER
    return __popcnt64(a ^ b);
#else
    return __builtin_popcountll(a ^ b);
#endif
}

cv::Mat dedup::loadMat(const QByteArray &bytes) {
    if (bytes.isEmpty()) return cv::Mat();
    // copy into std::vector<uchar> for cv::imdecode
    std::vector<uchar> buf;
    buf.assign((uchar*)bytes.constData(), (uchar*)bytes.constData() + bytes.size());

    cv::Mat img = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    return img;
}
QByteArray dedup::readDataFromPath(const QString &path){
    QFile f(path);
    if (!f.exists()) {
        // 文件不存在
        return QByteArray{};
    }
    if (!f.open(QIODevice::ReadOnly)) {
        // 打开失败（权限/锁等）
        return QByteArray{};
    }
    return f.readAll();
}

cv::Mat dedup::readImageBGR(const QByteArray &bytes) {
    cv::Mat img = loadMat(bytes);
    if (img.empty()) return img;
    if (img.channels() == 4) {
        cv::Mat bgr;
        cv::cvtColor(img, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    }
    return img;
}

// 估算 quality 的辅助函数（基于 libjpeg 标准表最小二乘匹配）


static inline double jpegQualityToScale(int q) {
    q = std::clamp(q, 1, 100);
    return (q < 50) ? (5000.0 / q) : (200.0 - 2.0 * q);
}

static inline int scaledQuant(unsigned int stdVal, double scale) {
    int v = static_cast<int>((stdVal * scale + 50.0) / 100.0);
    return std::clamp(v, 1, 255);
}

static double tableError(const JQUANT_TBL* tbl,
                         const std::array<unsigned int, 64>& stdTbl,
                         double scale)
{
    if (!tbl) return std::numeric_limits<double>::infinity();

    double err = 0.0;
    for (int i = 0; i < 64; ++i) {
        const int actual = static_cast<int>(tbl->quantval[i]);
        const int predicted = scaledQuant(stdTbl[i], scale);

        const double diff = static_cast<double>(actual - predicted);

        // 归一化，避免大数值主导误差
        const double denom = std::max(1.0, static_cast<double>(std::max(actual, predicted)));
        err += (diff * diff) / (denom * denom);
    }

    return err / 64.0;
}

JpegQualityEstimate dedup::estimateJpegQuality(jpeg_decompress_struct* cinfo) {
    static const std::array<unsigned int, 64> kStdLuma = {
        16, 11, 10, 16, 24, 40, 51, 61,
        12, 12, 14, 19, 26, 58, 60, 55,
        14, 13, 16, 24, 40, 57, 69, 56,
        14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68,109,103, 77,
        24, 35, 55, 64, 81,104,113, 92,
        49, 64, 78, 87,103,121,120,101,
        72, 92, 95, 98,112,100,103, 99
    };

    static const std::array<unsigned int, 64> kStdChroma = {
        17, 18, 24, 47, 99, 99, 99, 99,
        18, 21, 26, 66, 99, 99, 99, 99,
        24, 26, 56, 99, 99, 99, 99, 99,
        47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99
    };

    JpegQualityEstimate result{};

    if (!cinfo) return result;

    const JQUANT_TBL* lumaTbl = cinfo->quant_tbl_ptrs[0];
    const JQUANT_TBL* chromaTbl = cinfo->quant_tbl_ptrs[1];

    if (!lumaTbl && !chromaTbl) {
        return result;
    }

    double bestError = std::numeric_limits<double>::infinity();
    int bestQ = -1;

    for (int q = 1; q <= 100; ++q) {
        const double scale = jpegQualityToScale(q);

        double err = 0.0;
        int usedTables = 0;

        if (lumaTbl) {
            err += tableError(lumaTbl, kStdLuma, scale);
            ++usedTables;
        }
        if (chromaTbl) {
            err += tableError(chromaTbl, kStdChroma, scale);
            ++usedTables;
        }

        if (usedTables == 0) continue;
        err /= usedTables;

        if (err < bestError) {
            bestError = err;
            bestQ = q;
        }
    }

    result.quality = bestQ;
    result.error = bestError;

    // 经验阈值：越小越像“标准 IJG/libjpeg 质量缩放表”
    // exact match 时 error = 0
    result.likelyStandardIJG = (bestQ != -1 && bestError < 0.002);

    // 置信度只是工程上的启发式指标，不是数学意义上的概率
    if (bestQ != -1 && std::isfinite(bestError)) {
        result.confidence = std::exp(-bestError * 14.0);
        result.confidence = std::clamp(result.confidence, 0.0, 1.0);
    }

    return result;
}
/*
 * 返回: tuple(grayDiff, pixelDiffRatio, meanDiff)
 * - grayDiff: CV_8UC1, per-pixel absdiff magnitude (0..255)
 * - pixelDiffRatio: double (0..1) = fraction of pixels with grayDiff > thresh
 * - meanDiff: double (0..1) = mean(grayDiff)/255.0
 */
std::tuple<cv::Mat, double, double> dedup::computePixelDiff(const cv::Mat &aBGR_in, const cv::Mat &bBGR_in, double threshVal) {
    cv::Mat A = ensureBGR8(aBGR_in);
    cv::Mat B = ensureBGR8(bBGR_in);
    if (A.empty() || B.empty()) return {cv::Mat(), 1.0, 1.0};

    // Resize B to A (maintain content). If you prefer letterbox/pad, change here.
    if (A.size() != B.size()) {
        // choose interpolation: INTER_AREA when shrinking, INTER_LINEAR when enlarging
        int interpolation = (B.cols > A.cols || B.rows > A.rows) ? cv::INTER_AREA : cv::INTER_LINEAR;
        cv::resize(B, B, A.size(), 0, 0, interpolation);
    }

    cv::Mat diff;
    cv::absdiff(A, B, diff); // CV_8UC3
    cv::Mat gray;
    cv::cvtColor(diff, gray, cv::COLOR_BGR2GRAY); // CV_8UC1, 0..255

    // mean difference (normalized)
    cv::Scalar m = cv::mean(gray);
    double meanDiff = (m[0] / 255.0); // 0..1

    /*
     * 差异像素占比。
     *
     * threshVal <= 0 时不需要额外造一张二值掩码:
     *   THRESH_BINARY(0) 的语义是 th = (gray > 0) ? 255 : 0,
     *   所以 countNonZero(th) 恒等于 countNonZero(gray)。
     * 32MP 下这张掩码就是 31MB,批量扫描时每个任务都省一张,直接压低内存峰值
     * (峰值太高会让分配失败,失败又会被上层静默吞掉变成 pix diff = 1)。
     */
    double nonZero = 0.0;
    double total = (double)(gray.rows * gray.cols);
    if (threshVal <= 0.0) {
        nonZero = (double)cv::countNonZero(gray);
    } else {
        cv::Mat th;
        cv::threshold(gray, th, threshVal, 255, cv::THRESH_BINARY);
        nonZero = (double)cv::countNonZero(th);
    }
    double pixelDiffRatio = (total > 0.0) ? (nonZero / total) : 1.0;

    return {gray, pixelDiffRatio, meanDiff};
}

QImage dedup::cvMatToQImageRGBA(const cv::Mat &src) {
    if (src.empty()) return QImage();
    cv::Mat rgba;
    if (src.channels() == 1) {
        cv::cvtColor(src, rgba, cv::COLOR_GRAY2RGBA);
    } else if (src.channels() == 3) {
        cv::cvtColor(src, rgba, cv::COLOR_BGR2RGBA);
    } else if (src.channels() == 4) {
        cv::cvtColor(src, rgba, cv::COLOR_BGRA2RGBA);
    } else {
        return QImage();
    }
    return QImage((const uchar*)rgba.data, rgba.cols, rgba.rows, rgba.step, QImage::Format_RGBA8888).copy();
}

QImage dedup::cvMatToQImageRGB(const cv::Mat &src) {
    if (src.empty()) return QImage();
    cv::Mat rgb;
    if (src.channels() == 3) {
        cv::cvtColor(src, rgb, cv::COLOR_BGR2RGB);
    } else if (src.channels() == 1) {
        cv::cvtColor(src, rgb, cv::COLOR_GRAY2RGB);
    } else if (src.channels() == 4) {
        cv::cvtColor(src, rgb, cv::COLOR_BGRA2RGB);
    } else return QImage();
    return QImage((const uchar*)rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888).copy();
}

QImage dedup::cvMatToQImageRGB32(const cv::Mat &src)
{
    if (src.empty()) return QImage();
    if (src.depth() != CV_8U) return QImage();

    cv::Mat bgra;
    switch (src.channels()) {
    case 1:
        cv::cvtColor(src, bgra, cv::COLOR_GRAY2BGRA);
        break;
    case 3:
        cv::cvtColor(src, bgra, cv::COLOR_BGR2BGRA);   // alpha 自动置 255
        break;
    case 4: {
        // QImage::Format_RGB32 没有 alpha,先压成 3 通道再扩回来
        cv::Mat bgr;
        cv::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
        break;
    }
    default:
        return QImage();
    }
    // BGRA 的内存排布就是 0xffRRGGBB,和 Format_RGB32 一致
    return QImage(reinterpret_cast<const uchar*>(bgra.data), bgra.cols, bgra.rows,
                  static_cast<int>(bgra.step), QImage::Format_RGB32).copy();
}

QImage dedup::cvMatToDisplayQImage(const cv::Mat &src)
{
    if (src.empty()) return QImage();

    /*
     * ============ 先统一位深/通道,再谈显示 ============
     *
     * 扫描/建对只关心像素内容:16 位 PNG、gray+alpha 的 2 通道 PNG 都能正常
     * 解码、算 pHash、跑 ORB,于是它们会出现在 pair 列表里。
     * 但显示这一路以前只接受 CV_8U 且 1/3/4 通道 —— 遇到这些格式会走到
     * cvMatToQImageRGB32 顶部的 `depth != CV_8U` 直接返回空 QImage。
     * 后台任务看到 imgA/imgB 为空就 return,也不告诉界面,
     * 结果就是【某一对图怎么切都不上屏】,而其它对一切正常。
     *
     * 这里把能显示的都归一化到 8U,并且把 2 通道(灰度+alpha)补成 BGRA;
     * 真正的 alpha 判断仍然交给下面那段(它只处理 4x8U),
     * 所以不会破坏"不透明走 RGB32 快速路径"的优化。
     */
    cv::Mat work = src;
    if (work.depth() != CV_8U) {
        switch (work.depth()) {
        case CV_16U:
            // 65535 -> 255;这是 16 位 PNG/TIFF 最常见的映射
            work.convertTo(work, CV_8U, 1.0 / 257.0);
            break;
        case CV_16S:
            work.convertTo(work, CV_8U, 1.0 / 256.0, 128.0);
            break;
        case CV_32F:
        case CV_64F:
            // 绝大多数是 [0,1] 的浮点图;超出范围的由 convertTo 饱和截断
            work.convertTo(work, CV_8U, 255.0);
            break;
        default:
            return QImage();
        }
    }

    /*
     * 2 通道 = 灰度 + alpha(PNG 常见)。
     * cv::cvtColor 没有 GRAY+ALPHA -> BGRA 的一步转换,拆开做:
     * 灰度扩成 BGR,再把第二通道塞回 alpha 位。
     */
    if (work.channels() == 2) {
        std::vector<cv::Mat> ch;
        cv::split(work, ch);
        cv::Mat bgr, bgra;
        cv::cvtColor(ch[0], bgr, cv::COLOR_GRAY2BGR);
        cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
        cv::insertChannel(ch[1], bgra, 3);
        work = bgra;
    }

    if (work.channels() != 1 && work.channels() != 3 && work.channels() != 4)
        return QImage();

    /*
     * 只有 4 通道 8 位才可能存在真正的透明像素。
     * 这里抽一遍 alpha 通道看最小值是不是 255:
     *   - 是   -> 全不透明,走 RGB32(UI 线程零转换,而且绘制时不会走棋盘格);
     *   - 不是 -> 真的有透明区域,保留 RGBA 让显示控件画棋盘格。
     * 这一步(抽通道 + minMaxLoc)在后台做,32MP 实测 29ms,和它省下的
     * 每次切图 24ms x2 相比是划算的 —— 更别说 highlight 时还会反复重绘。
     */
    if (work.channels() == 4) {
        try {
            cv::Mat alpha;
            cv::extractChannel(work, alpha, 3);
            double mn = 0.0, mx = 0.0;
            cv::minMaxLoc(alpha, &mn, &mx);
            if (mn < 255.0)
                return cvMatToQImageRGBA(work);

            /*
             * 全不透明分支:BGRA 在内存里的字节序就是 [B][G][R][A],
             * A 又恰好全是 255 —— 这正是 QImage::Format_RGB32 要求的 0xffRRGGBB 排布,
             * 所以【直接拷一次】就是合法的 RGB32 图,不需要任何颜色转换。
             *
             * 走 cvMatToQImageRGB32 的话要 BGRA->BGR->BGRA 两遍转换再拷一次,
             * 32MP 实测 78ms;这里一次拷贝,实测 ~39ms。
             * (alpha 全 255 已经由上面 minMaxLoc 保证,不是"假设"。)
             */
            QImage fast(work.cols, work.rows, QImage::Format_RGB32);
            const size_t rowBytes = static_cast<size_t>(work.cols) * 4;
            const bool tightStride = work.isContinuous()
                                     && static_cast<size_t>(fast.bytesPerLine()) == rowBytes;
            if (tightStride) {
                std::memcpy(fast.bits(), work.data, rowBytes * static_cast<size_t>(work.rows));
            } else {
                for (int y = 0; y < work.rows; ++y)
                    std::memcpy(fast.scanLine(y), work.ptr(y), rowBytes);
            }
            return fast;
        } catch (...) {
            // 判断失败就退回 RGB32(不透明),不影响功能
        }
    }
    return cvMatToQImageRGB32(work);
}

void dedup::alignPairToCommonSize(const cv::Mat &A, const cv::Mat &B, cv::Mat &outA, cv::Mat &outB)
{
    if (A.empty() && B.empty()) { outA = A; outB = B; return; }
    if (A.empty()) { outA = cv::Mat::zeros(B.size(), B.type()); outB = B.clone(); return; }
    if (B.empty()) { outA = A.clone(); outB = cv::Mat::zeros(A.size(), A.type()); return; }

    const int targetW = std::max(A.cols, B.cols);
    const int targetH = std::max(A.rows, B.rows);

    auto prepare = [&](const cv::Mat &src, cv::Mat &dst){
        // 尺寸正好等于公共画布:一次拷贝就够(copyTo 会自己按需分配 dst)
        if (src.cols == targetW && src.rows == targetH) {
            src.copyTo(dst);
            return;
        }

        /*
         * 缩放系数取两轴里更小的那个(因为 target 取 max,所以 s >= 1):
         *   - 两条边都比对方小 -> 放大到"其中一条边先够到对方",另一条边补边;
         *   - 一条边更大、一条更小 -> s == 1,完全不缩放,只补边。
         * 关键是【每条分支都必须补到 targetW×targetH】:
         * 旧代码在 s≈1 时直接 copyTo 不补边,再靠调用方 "resize 到同一尺寸" 收尾,
         * 于是那张图被硬拉变形(用户报的"图片对里一张图的长或宽被拉到变形")。
         */
        const double sx = (double)targetW / src.cols;
        const double sy = (double)targetH / src.rows;
        const double s  = std::min(sx, sy);

        cv::Mat canvas;
        if (std::fabs(s - 1.0) > 1e-9) {
            cv::resize(src, canvas, cv::Size(), s, s, cv::INTER_LINEAR);
        } else {
            canvas = src;
        }

        const int padW = targetW - canvas.cols;
        const int padH = targetH - canvas.rows;
        const int left = padW / 2, right = padW - left;
        const int top  = padH / 2, bottom = padH - top;
        cv::copyMakeBorder(canvas, dst, top, bottom, left, right, cv::BORDER_CONSTANT, cv::Scalar(0,0,0));
    };

    prepare(A, outA);
    prepare(B, outB);

    if (outA.size() != outB.size()) {
        /*
         * 兜底:理论上到不了这里(上面每条分支都会补成 targetW×targetH)。
         * 万一到了也用补边对齐 —— 绝不 resize 硬拉,那正是长宽比失真的来源。
         */
        cv::Mat tmpA, tmpB;
        prepare(outA, tmpA);
        prepare(outB, tmpB);
        outA = tmpA;
        outB = tmpB;
    }
}

QStringList dedup::supportedExtensions() {
    return QStringList{ "png","jpe","jpeg","jpg","bmp","webp" };
}

bool dedup::isUnderOrEqual(const QString &path, const QString &dir)
{
    if (path.isEmpty() || dir.isEmpty()) return false;

    QString p = QDir::toNativeSeparators(QDir::cleanPath(path));
    QString d = QDir::toNativeSeparators(QDir::cleanPath(dir));
    p.replace('\\', '/');
    d.replace('\\', '/');
    // 去掉 dir 结尾的斜杠(注意盘符根目录 "E:/" 会变成 "E:")
    while (d.endsWith(QLatin1Char('/')))
        d.chop(1);
    if (d.isEmpty()) return false;

    if (p.compare(d, Qt::CaseInsensitive) == 0) return true;
    return p.startsWith(d + QLatin1Char('/'), Qt::CaseInsensitive);
}

QString dedup::topLevelSubDirOf(const QString &rootFolder, const QString &filePath)
{
    if (rootFolder.isEmpty() || filePath.isEmpty()) return QString();

    QString rel = QDir(rootFolder).relativeFilePath(filePath);
    rel.replace('\\', '/');
    rel = QDir::cleanPath(rel);
    if (rel.isEmpty() || rel == QLatin1String(".")) return QString();
    // 不在 rootFolder 之下(例如换了盘符)就谈不上"顶层子目录"
    if (rel.startsWith(QLatin1String(".."))) return QString();

    const int slash = rel.indexOf(QLatin1Char('/'));
    if (slash < 0) return QString();          // 直接放在 rootFolder 下
    return rel.left(slash);
}


cv::Mat dedup::ensureBGR8(const cv::Mat &src) {
    if (src.empty()) return cv::Mat();
    cv::Mat out;
    int ch = src.channels();
    int depth = src.depth();
    /*
     * 已经是 8UC3 就直接把原矩阵交出去(引用计数,零拷贝)。
     *
     * 这里原来是 `return src.clone();` —— 每次白拷一整张图。
     * 32MP(4626x7016)一张就是 93MB,而 computePixelDiff 每个任务要调两次,
     * 批量扫描时这个 clone 是纯浪费:调用方(computePixelDiff / makeThumbnail)
     * 全都只读,不会写这两个矩阵。
     * 而且它抬高了瞬时内存峰值 —— 内存不够时分配失败会被上层静默吞掉,
     * 表现就是"一堆图片对 pix diff 变成 1"。
     */
    if (src.type() == CV_8UC3) return src;
    if (ch == 4) {
        if (depth != CV_8U) {
            cv::Mat tmp; src.convertTo(tmp, CV_8U);
            cv::cvtColor(tmp, out, cv::COLOR_BGRA2BGR);
        } else {
            cv::cvtColor(src, out, cv::COLOR_BGRA2BGR);
        }
    } else if (ch == 1) {
        if (depth != CV_8U) {
            cv::Mat tmp; src.convertTo(tmp, CV_8U);
            cv::cvtColor(tmp, out, cv::COLOR_GRAY2BGR);
        } else {
            cv::cvtColor(src, out, cv::COLOR_GRAY2BGR);
        }
    } else if (ch == 2) {
        // rare: treat as gray + alpha -> convert to gray then to BGR
        cv::Mat tmp;
        if (depth != CV_8U) src.convertTo(tmp, CV_8U); else tmp = src;
        cv::cvtColor(tmp, out, cv::COLOR_GRAY2BGR);
    } else {
        // fallback: convert depth then try split/merge first 3 channels
        cv::Mat tmp;
        if (depth != CV_8U) src.convertTo(tmp, CV_8U); else tmp = src;
        std::vector<cv::Mat> chs;
        cv::split(tmp, chs);
        if (chs.size() >= 3) {
            std::vector<cv::Mat> sel{chs[0], chs[1], chs[2]};
            cv::merge(sel, out);
        } else {
            cv::cvtColor(tmp, out, cv::COLOR_GRAY2BGR);
        }
    }
    return out;
}
