#include "fonts.h"

#include <QFontDatabase>
#include <QStringList>

const QString& appFontFamily()
{
    static const QString family = [] {
        // Ubuntu 三字重(Light/Regular/Bold)一起注册,合并出 "Ubuntu" family:
        // 主字重是 Light,需要粗体的地方能命中真 Bold 而不是合成粗体。
        // 两个程序的资源前缀不同,按顺序尝试。
        static const char* const kPrefixes[] = {":/res/fonts/", ":/fonts/"};
        for (const char* prefix : kPrefixes) {
            int loaded = 0;
            for (const char* file : {"Ubuntu-Light.ttf", "Ubuntu-Regular.ttf", "Ubuntu-Bold.ttf"})
                if (QFontDatabase::addApplicationFont(QString::fromLatin1(prefix) + QLatin1String(file)) >= 0)
                    ++loaded;
            if (loaded == 3)
                return QStringLiteral("Ubuntu");
        }
        return QStringLiteral("Segoe UI");
    }();
    return family;
}
