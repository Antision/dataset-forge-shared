#include "paircontextmenu.h"

#include "deduppch.h"

#include <QAction>
#include <QMenu>

namespace {

/*
 * 菜单样式 = imagededuplicatewindow.ui 里那段 QMenu QSS 的拷贝。
 *
 * 直接写在这里是为了让"列表行右键"和"按钮右键"用同一份规则 ——
 * 按钮是子控件,菜单以它为 parent 时继承到的 QSS 不一定一样,显式设一份最稳。
 */
const char *kMenuQss = R"(
QMenu {
    background-color: #000000;
    color: #cacaca;
    border: 1px solid #383838;
    border-radius: 2px;
    padding: 2px;
}
QMenu::item {
    padding: 8px 28px;
    border-radius: 4px;
    background-color: transparent;
}
QMenu::item:selected {
    background-color: #202538;
    color: white;
}
QMenu::item:disabled {
    color: #777D89;
}
QMenu::separator {
    height: 1px;
    background-color: #414550;
    margin: 5px 8px;
}
)";

} // namespace

namespace pairmenu {

void exec(QWidget *anchor, const QPoint &globalPos, const QVector<Section> &sections)
{
    QMenu menu(anchor);
    menu.setStyleSheet(QString::fromUtf8(kMenuQss));

    std::function<void()> chosen;
    bool anyAdded = false;

    for (const Section &sec : sections) {
        if (sec.isEmpty())
            continue;
        if (anyAdded)
            menu.addSeparator();     // 段间分隔线(竖向排列,一条横线把两组分开)
        for (const Entry &e : sec) {
            QAction *act = menu.addAction(e.text);
            act->setEnabled(e.enabled);
            std::function<void()> trig = e.trigger;
            QObject::connect(act, &QAction::triggered, &menu,
                             [&chosen, trig]() { chosen = trig; });
        }
        anyAdded = true;
    }

    if (!anyAdded)
        return;

    menu.exec(globalPos);
    // 菜单已关闭:再执行动作,避免在嵌套事件循环里改 m_pairs / 搬文件
    if (chosen)
        chosen();
}

} // namespace pairmenu
