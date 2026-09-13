#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

// ---------------------------------------------------------------------------
// drag_probe —— 拖放行为实测探针（一次性实验，不是产品代码）。
//
// 为什么要做这个实验：架构方案 §6 承认「把文件从程序拖到资源管理器时，
// Windows 到底是复制还是移动」无法从文档和 Qt 头文件确定。
// Qt 的公开 API 没有暴露 CFSTR_PREFERREDDROPEFFECT，而相关的
// QWindowsMimeConverter 是私有头（_P_H），不能依赖。
//
// 这个结论直接决定「浮窗拖出还原」能不能按需求方的预期实现，
// 所以必须先实测，不能靠猜。
//
// 判定标准：
//   * 返回 MoveAction 且源文件消失  -> 情形 B，拖出即移动，功能可完整实现；
//   * 返回 CopyAction 且源文件仍在  -> 情形 A，只是复制，需要降级方案；
//   * 返回 IgnoreAction            -> 对方没接受，要检查 mime 格式。
// ---------------------------------------------------------------------------

class ProbeList : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions);

        QListWidgetItem *item = currentItem();
        if (!item)
            return;

        const QString path = item->data(Qt::UserRole).toString();

        auto *mime = new QMimeData;
        mime->setUrls({ QUrl::fromLocalFile(path) });
        // 私有标记：产品里用它区分"这是 DeskTidy 的还原拖拽"
        mime->setData(QStringLiteral("application/x-desktidy-restore"),
                      path.toUtf8());

        auto *drag = new QDrag(this);
        drag->setMimeData(mime);

        QTextStream out(stdout);
        out << "\n=== drag started ===\n";
        out << "source: " << path << "\n";
        out << "source exists before: " << (QFile::exists(path) ? "yes" : "no") << "\n";
        out.flush();

        // 同时允许 Copy 与 Move，让 shell 自己决定默认行为
        const Qt::DropAction result =
            drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);

        const bool stillThere = QFile::exists(path);

        out << "--- drag finished ---\n";
        out << "exec() returned: ";
        switch (result) {
        case Qt::CopyAction:   out << "CopyAction\n";   break;
        case Qt::MoveAction:   out << "MoveAction\n";   break;
        case Qt::LinkAction:   out << "LinkAction\n";   break;
        case Qt::IgnoreAction: out << "IgnoreAction\n"; break;
        default:               out << "unknown(" << int(result) << ")\n"; break;
        }
        out << "source exists after: "
            << (stillThere ? "yes (not moved)" : "no (moved away)") << "\n";

        out << ">>> VERDICT: ";
        if (!stillThere) {
            out << "CASE B -- explorer MOVED the file. "
                   "'drop anywhere = restore' is fully achievable.\n";
        } else if (result == Qt::CopyAction) {
            out << "CASE A -- only COPIED, source still in box. "
                   "Needs a degraded design.\n";
        } else {
            out << "needs manual judgement.\n";
        }
        out << "=== done ===\n\n";
        out.flush();

        // 落盘结果，方便自动化读取
        QFile log(QStringLiteral("F:/QtProject/DeskTidy/_drag_probe_result.txt"));
        if (log.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream f(&log);
            f << "result=" << int(result) << "\n";
            f << "sourceExists=" << (stillThere ? "true" : "false") << "\n";
            f << "path=" << path << "\n";
        }
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString probeDir = QStringLiteral("F:/QtProject/DeskTidy/_drag_probe");
    QDir().mkpath(probeDir);
    const QString srcFile = probeDir + QStringLiteral("/probe_file.txt");

    QFile f(srcFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write("desktidy drag probe payload\n");
        f.close();
    }

    QWidget w;
    auto *layout = new QVBoxLayout(&w);

    auto *hint = new QLabel(&w);
    hint->setText(QStringLiteral(
        "【DeskTidy 拖放行为实测】\n\n"
        "请把下面这个文件拖到桌面（或任意资源管理器文件夹）。\n"
        "程序会记录：对方要的是复制还是移动、源文件有没有消失。"));
    layout->addWidget(hint);

    auto *list = new ProbeList(&w);
    list->setDragEnabled(true);
    list->setDragDropMode(QAbstractItemView::DragOnly);
    auto *item = new QListWidgetItem(QFileInfo(srcFile).fileName(), list);
    item->setData(Qt::UserRole, srcFile);
    list->setCurrentItem(item);
    layout->addWidget(list);

    w.resize(560, 220);
    w.setWindowTitle(QStringLiteral("DeskTidy drag probe"));
    w.show();

    // 无人操作时自动退出，避免挂死
    QTimer::singleShot(300000, &app, &QApplication::quit);

    return app.exec();
}
