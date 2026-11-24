#ifndef CACHEIO_H
#define CACHEIO_H

/*
 * image_features.tscch 读取/写入
 *
 * 文件格式(QDataStream,**大端**,Qt_6_0):
 *   quint32 magic        'T''S''C''1'
 *   quint16 version      当前写出 4;读取接受 4~5
 *                        4 = ORB 改为"长边1024 / 800特征",并保存关键点坐标
 *                        5 = 已废弃:记录尾部多 16 字节(两个恒为 -1 的 double),
 *                            读取时只跳过、不解析,写回即降级为 4
 *   quint32 flags        保留(0)
 *   quint64 savedAt      上一次【完整】扫描该目录的时间(ms since epoch)
 *                        注意:部分更新(只覆盖了该 tscch 作用域的一部分)不会改它,
 *                        按正则筛选的扫描也绝不会改它。
 *                        目前用于日志/诊断(读缓存时会把时间打出来)和"保留语义",
 *                        还没有被用作"跳过扫描"的依据 —— 因为只凭目录 mtime 判断
 *                        "子树没变过"是有风险的(原地改内容不会改目录 mtime),
 *                        所以宁可老实 stat 每个文件,也不拿它做偷懒判断。
 *   quint32 entryCount
 *   重复entryCount次:
 *     QString  relPath   相对于该tscch所在目录的相对路径,统一使用 '/' 分隔,
 *                        且以 "./" 开头(例如 "./2_new/x.jpg")
 *     quint64  filesize
 *     qint64   modTime   最后修改时间(ms since epoch),用于快速判断缓存是否失效
 *     quint32  width, height
 *     QString  format    真实格式(按文件头判断):jpg/png/webp/gif/bmp/tif
 *     qint32   quality   jpg=量化表估计值;png/无损webp=101;有损webp=VP8估计值;-1=无法估计
 *     quint64  pHash
 *     quint64  dHash
 *     qint32   orbKeypoints
 *     char[32] SHA256
 *     quint32  descRows       ORB描述子行数(0表示没有描述子)
 *     quint32  descCols
 *     quint32  descType       cv::Mat::type()(实际只会是CV_8U)
 *     raw bytes descRows*descCols*elemSize
 *     quint32  ptRows         关键点坐标行数(必须等于 descRows,否则整对作废)
 *     quint32  ptCols         固定2
 *     quint32  ptType         固定CV_32F
 *     raw bytes ptRows*ptCols*elemSize
 *   quint32  subDirCount
 *   重复subDirCount次:
 *     QString  subDirName   顶层子目录名(空串代表"直接位于本目录下的文件")
 *     quint64  updatedAt    上次更新该子目录条目时,这个子目录本身的 mtime
 *
 * 注意:缩略图不写入文件(按用户要求),运行时由 makeThumbnail()/computeFileFeatures() 生成。
 *
 * 部分更新(扫描 ./b/ 时回写上一层的 ./image_features.tscch)靠 subDirUpdatedAt 记录
 * "b 这个顶层子目录是什么时候更新的",从而在不重写整个文件的前提下也能判断是否需要更新。
 * (目前它同样是"记录下来 + 写进去",作为部分更新的审计依据;
 *  真正的"不许覆盖别人记录"是靠 WriteOptions::mergeOnly / pruneWithin 保证的。)
 *
 * ⚠ 字节序:QDataStream 的默认字节序是 **BigEndian**,这里从未调过 setByteOrder(),
 *   所以文件里所有整数(含 QString 的长度前缀、UTF-16 的每个码元)都是大端。
 *   以前这里写成"小端",害得外部解析器按小端去读 —— 校验方法:文件前 4 字节是
 *   'T''S''C''1',紧接着的 version 是 00 04(大端=4),小端会读成 1024。
 *   外部读写这份缓存请按大端来(scripts/flatten_folder_tree.py 就是按大端实现的)。
 */

#include "deduppch.h"
#include "dedupworker.h"

/*
 * cacheio 的接口(CacheMeta / WriteOptions / 三个函数)声明在 dedupworker.h 里,
 * 因为那边要用、而 cacheio 又要序列化 FileEntry,放在一起才没有循环依赖。
 * 这个头文件主要负责"格式说明"这一层,包含它就能拿到全部声明。
 */

#endif // CACHEIO_H
