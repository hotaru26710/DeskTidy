#include "preferreddropeffect.h"

// ---------------------------------------------------------------------------
// 实现说明
//
// * 只在 Windows 上编译真正的转换器；其他平台 ensureRegistered() 是空函数。
//
// * 转换器**只对 DeskTidy 自己的拖出生效**：formatsForMime 里检查 mimeType
//   是否为 application/x-desktidy-restore，不是就返回空列表。这样普通的
//   文件拖拽（例如从桌面拖进盒里）完全不受影响，行为与不加本模块时一致。
//
// * 关于 QWindowsMimeConverter 的三个纯虚函数分工：
//     canConvertFromMime / convertFromMime / formatsForMime —— Qt -> Windows
//     canConvertToMime / convertToMime / mimeForFormat    —— Windows -> Qt
//   我们只需要"往前送"这一个方向，故后三个一律返回 false / 空。
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN

#include <QMimeData>
#include <QString>

#include <objidl.h>
#include <oleidl.h>
#include <windows.h>

#include <QtGui/qwindowsmimeconverter.h>

namespace {

// 私有标记：它是"这次拖拽来自 DeskTidy 浮窗"的唯一凭据，
// 同时也是下面那个转换器生效的触发器（见 formatsForMime）。
// 与 ItemListWidget::startDrag 里写入的字符串必须严格一致。
const char *kRestoreMimeType = "application/x-desktidy-restore";

// Windows 用这个格式名协商"你希望我怎么处理这次拖放"。
// 名字是系统约定的固定字符串，不能改。
const char *kPreferredDropEffectFormat = "Preferred Drop Effect";

class PreferredDropEffectConverter : public QWindowsMimeConverter
{
public:
    // ---- Qt -> Windows ----

    bool canConvertFromMime(const FORMATETC &formatetc,
                            const QMimeData *mimeData) const override
    {
        Q_UNUSED(mimeData);
        return formatetc.cfFormat == m_registeredFormat;
    }

    bool convertFromMime(const FORMATETC &formatetc, const QMimeData *mimeData,
                         STGMEDIUM *pmedium) const override
    {
        Q_UNUSED(mimeData);

        if (formatetc.cfFormat != m_registeredFormat || !pmedium)
            return false;

        // 关键的一行：告诉 shell"这是一次移动操作"。
        // 改成 DROPEFFECT_COPY 就退化成默认的复制行为了。
        const DWORD effect = DROPEFFECT_MOVE;

        // 该格式约定用全局内存块承载一个 DWORD。
        // GMEM_SHARE 是 OLE 数据传输所要求的（跨进程访问）。
        HGLOBAL hMem = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE, sizeof(DWORD));
        if (!hMem)
            return false;

        void *ptr = GlobalLock(hMem);
        if (!ptr) {
            GlobalFree(hMem);       // 锁不上就得把块还回去，否则泄漏
            return false;
        }
        memcpy(ptr, &effect, sizeof(DWORD));
        GlobalUnlock(hMem);

        pmedium->tymed          = TYMED_HGLOBAL;
        pmedium->hGlobal        = hMem;
        pmedium->pUnkForRelease = nullptr;
        return true;
    }

    QList<FORMATETC> formatsForMime(const QString &mimeType,
                                    const QMimeData *mimeData) const override
    {
        // ⚠️ 这是"不污染普通拖拽"的关键闸门：
        // 只有当 mime 里带着我们的私有标记时才附加 Preferred Drop Effect。
        // 若在这里无脑返回该格式，所有经过本程序的拖拽都会被标成"移动"，
        // 包括主人从桌面拖文件进盒子的那一次 —— 那会导致源文件被系统删除。
        if (mimeType != QLatin1String(kRestoreMimeType) || !mimeData)
            return {};

        FORMATETC fmt = {};
        fmt.cfFormat = m_registeredFormat;
        fmt.ptd      = nullptr;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex   = -1;
        fmt.tymed    = TYMED_HGLOBAL;

        return { fmt };
    }

    // ---- Windows -> Qt：本模块不需要这个方向 ----

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

    // ---- 注册 ----

    // 把原生格式名换成 cfFormat 编号。整个进程生命周期内只做一次。
    static int registeredFormat()
    {
        static const int format = QWindowsMimeConverter::registerMimeType(
            QLatin1String(kPreferredDropEffectFormat));
        return format;
    }

    static PreferredDropEffectConverter *instance()
    {
        // 注册后由 Qt 持有并负责析构（QWindowsMimeConverter 的约定），
        // 故这里不 delete，也不需要智能指针。
        static PreferredDropEffectConverter *converter =
            new PreferredDropEffectConverter(registeredFormat());
        return converter;
    }

private:
    explicit PreferredDropEffectConverter(int format)
        : m_registeredFormat(format)
    {
    }

    int m_registeredFormat = 0;
};

} // namespace

namespace PreferredDropEffect {

void ensureRegistered()
{
    // 第一次调用会创建并注册转换器；后续调用命中函数内 static 直接返回。
    PreferredDropEffectConverter::instance();
}

} // namespace PreferredDropEffect

#else // !Q_OS_WIN

// 非 Windows 平台：拖拽默认就是移动语义，无需任何处理。
// 提供空实现是为了让调用方（main.cpp）不必写平台条件编译。
namespace PreferredDropEffect {

void ensureRegistered()
{
}

} // namespace PreferredDropEffect

#endif // Q_OS_WIN
