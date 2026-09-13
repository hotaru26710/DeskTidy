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

#include <objidl.h>
#include <oleidl.h>
#include <windows.h>

#include <QtGui/qwindowsmimeconverter.h>

// ---------------------------------------------------------------------------
// drag_probe2 —— 拖放「移动语义」实测探针（第二轮实验）。
//
// 第一轮实验结论：Windows 资源管理器默认对拖入的文件执行**复制**，
// 源文件不会消失（exec() 返回 CopyAction=1）。
//
// 原因：Windows shell 依据剪贴板格式 CFSTR_PREFERREDDROPEFFECT 决定默认动作，
// 而 Qt 的 QMimeData 没有直接暴露设置它的接口。
//
// 本实验验证的假设：可以通过 QWindowsMimeConverter 注册一个自定义转换器，
// 在 Qt 把 QMimeData 翻译成 Windows IDataObject 时，**额外挂上
// CFSTR_PREFERREDDROPEFFECT = DROPEFFECT_MOVE**，从而让资源管理器
// 把拖入当成"移动"处理。
//
// 若成功：exec() 应返回 MoveAction，且源文件消失 —— 需求方要的
//         "拖到哪就还原到哪" 可以完整实现。
// 若失败：退回"复制 + 明确提示"的降级方案。
//
// 注意：本实验只在拖拽期间给剪贴板挂格式，不修改任何真实文件。
// ---------------------------------------------------------------------------

// 私有格式名：Windows 用这个格式名协商"你希望我怎么处理这次拖放"
static const char *kPreferredDropEffect = "Preferred Drop Effect";

class PreferredDropEffectMime : public QWindowsMimeConverter
{
public:
    // Qt -> Windows：把我们的意图翻译成原生格式
    bool canConvertFromMime(const FORMATETC &formatetc, const QMimeData *mimeData) const override
    {
        Q_UNUSED(mimeData);
        return formatetc.cfFormat == m_registeredFormat;
    }

    bool convertFromMime(const FORMATETC &formatetc, const QMimeData *mimeData,
                         STGMEDIUM *pmedium) const override
    {
        if (formatetc.cfFormat != m_registeredFormat || !pmedium)
            return false;

        // 关键：告诉 shell "这是一次移动操作"
        DWORD effect = DROPEFFECT_MOVE;

        // 用全局内存块承载这个 DWORD
        HGLOBAL hMem = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE, sizeof(DWORD));
        if (!hMem)
            return false;

        void *ptr = GlobalLock(hMem);
        if (!ptr) {
            GlobalFree(hMem);
            return false;
        }
        memcpy(ptr, &effect, sizeof(DWORD));
        GlobalUnlock(hMem);

        pmedium->tymed          = TYMED_HGLOBAL;
        pmedium->hGlobal        = hMem;
        pmedium->pUnkForRelease = nullptr;

        Q_UNUSED(mimeData);
        return true;
    }

    QList<FORMATETC> formatsForMime(const QString &mimeType,
                                    const QMimeData *mimeData) const override
    {
        // 只在我们自己的私有 mime 出现时，才附加 Preferred Drop Effect
        if (mimeType != QStringLiteral("application/x-desktidy-restore") || !mimeData)
            return {};

        FORMATETC fmt = {};
        fmt.cfFormat = m_registeredFormat;
        fmt.ptd      = nullptr;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex   = -1;
        fmt.tymed    = TYMED_HGLOBAL;

        return { fmt };
    }

    bool canConvertToMime(const QString &mimeType, IDataObject *pDataObj) const override
    {
        Q_UNUSED(mimeType);
        Q_UNUSED(pDataObj);
        return false;
    }

    QVariant convertToMime(const QString &mimeType, IDataObject *pDataObj,
                           QMetaType preferredType) const override
    {
        Q_UNUSED(mimeType);
        Q_UNUSED(pDataObj);
        Q_UNUSED(preferredType);
        return {};
    }

    QString mimeForFormat(const FORMATETC &formatetc) const override
    {
        Q_UNUSED(formatetc);
        return {};
    }

    static QWindowsMimeConverter *instance()
    {
        static QWindowsMimeConverter *conv = new PreferredDropEffectMime;
        return conv;
    }

    static void ensureRegistered()
    {
        instance();
        // 注册原生格式名，拿到对应的 cfFormat 编号
        m_registeredFormat = QWindowsMimeConverter::registerMimeType(
            QString::fromLatin1(kPreferredDropEffect));
    }

private:
    static int m_registeredFormat;
};

int PreferredDropEffectMime::m_registeredFormat = 0;

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
        // 私有 mime 是触发器：只有它出现，我们的 converter 才会附加
        // Preferred Drop Effect 格式
        mime->setData(QStringLiteral("application/x-desktidy-restore"),
                      path.toUtf8());

        auto *drag = new QDrag(this);
        drag->setMimeData(mime);

        QTextStream out(stdout);
        out << "\n=== round 2: drag started ===\n";
        out << "source: " << path << "\n";
        out << "source exists before: " << (QFile::exists(path) ? "yes" : "no") << "\n";
        out.flush();

        const Qt::DropAction result =
            drag->exec(Qt::MoveAction, Qt::MoveAction);

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
        if (!stillThere)
            out << "SUCCESS -- MOVE semantics achieved.\n";
        else if (result == Qt::MoveAction)
            out << "PARTIAL -- shell reported MoveAction but source still present.\n";
        else
            out << "FAILED -- still copy semantics; fall back to degraded design.\n";
        out << "=== done ===\n\n";
        out.flush();

        QFile log(QStringLiteral("F:/QtProject/DeskTidy/_drag_probe2_result.txt"));
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

    PreferredDropEffectMime::ensureRegistered();

    const QString probeDir = QStringLiteral("F:/QtProject/DeskTidy/_drag_probe2");
    QDir().mkpath(probeDir);
    const QString srcFile = probeDir + QStringLiteral("/probe2_file.txt");

    QFile f(srcFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write("desktidy drag probe 2 payload\n");
        f.close();
    }

    QWidget w;
    auto *layout = new QVBoxLayout(&w);

    auto *hint = new QLabel(&w);
    hint->setText(QStringLiteral(
        "【DeskTidy 拖放实测 · 第二轮】\n\n"
        "请把下面这个文件拖到桌面上一个**空文件夹**里（建个临时文件夹更清楚）。\n\n"
        "本轮验证：能否让 Windows 把它当成“移动”。\n"
        "若成功，源文件会消失；若失败，源文件还在。"));
    layout->addWidget(hint);

    auto *list = new ProbeList(&w);
    list->setDragEnabled(true);
    list->setDragDropMode(QAbstractItemView::DragOnly);
    auto *item = new QListWidgetItem(QFileInfo(srcFile).fileName(), list);
    item->setData(Qt::UserRole, srcFile);
    list->setCurrentItem(item);
    layout->addWidget(list);

    w.resize(620, 250);
    w.setWindowTitle(QStringLiteral("DeskTidy drag probe 2"));
    w.show();

    QTimer::singleShot(300000, &app, &QApplication::quit);

    return app.exec();
}
