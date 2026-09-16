#include "settings.h"

#include <algorithm>

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

// ---------------------------------------------------------------------------
// Settings 实现。
//
// 只用 IniFormat 落在 %APPDATA%\DeskTidy\DeskTidy.ini。
// 刻意不用注册表：主人想看一眼、想删掉配置重置，直接找到文件即可，
// 不必去翻 regedit。
// ---------------------------------------------------------------------------

namespace {

// 配置键名集中在此，避免读写两侧拼写不一致的经典事故。
const QString kKeyExcluded    = QStringLiteral("filters/excludedNames");
const QString kKeyLastBox     = QStringLiteral("ui/lastBoxName");
const QString kKeyFloatBoxes  = QStringLiteral("floating/openBoxes");
const QString kKeyAlwaysOnTop = QStringLiteral("floating/alwaysOnTop");
const QString kKeyTrayHint    = QStringLiteral("ui/trayHintShown");

// 界面动画总开关。全局单键，默认开启（见 Settings::animationsEnabled 的说明：
// 这一项刻意"默认值也落键"，与每盒外观的"只存非默认值"约定不同）。
const QString kKeyAnimations  = QStringLiteral("ui/animations");

// 悬停自动展开总开关。同样是全局单键、同样"默认值也落键"，理由见上。
const QString kKeyHoverExpand = QStringLiteral("ui/hoverExpand");

// 开机自启方式偏好。真正的启动项在注册表里，这里只记住用户选过"静默"还是"普通"。
const QString kKeyAutoStartSilent = QStringLiteral("autostart/silent");

// 含盒名的那几个键用 %1 占位，盒名经编码后填入（见 encodeBoxName）。
const QString kKeyFloatGeomFmt =
    QStringLiteral("floating/geometry/%1");
const QString kKeyFloatRollFmt =
    QStringLiteral("floating/rolledUp/%1");

// 浮窗是否被"钉住"（不参与互相推开、固定在最底层）。每盒一份。
const QString kKeyFloatLockedFmt =
    QStringLiteral("floating/locked/%1");

// 浮窗外观（每盒一份，同样带盒名占位）。
const QString kKeyFloatViewModeFmt =
    QStringLiteral("floating/viewMode/%1");
const QString kKeyFloatIconSizeFmt =
    QStringLiteral("floating/iconSize/%1");
const QString kKeyFloatOpacityFmt =
    QStringLiteral("floating/opacity/%1");

// 悬停触感与动画（每盒一份）。默认值：Glow / Standard / Standard /
// 250ms / 400ms / 8px —— 与 BoxAppearance 的默认构造保持一致。
const QString kKeyFloatHoverEffectFmt =
    QStringLiteral("floating/hoverEffect/%1");
const QString kKeyFloatHoverStrengthFmt =
    QStringLiteral("floating/hoverStrength/%1");
const QString kKeyFloatAnimationSpeedFmt =
    QStringLiteral("floating/animationSpeed/%1");
const QString kKeyFloatHoverExpandDelayFmt =
    QStringLiteral("floating/hoverExpandDelay/%1");
const QString kKeyFloatHoverCollapseDelayFmt =
    QStringLiteral("floating/hoverCollapseDelay/%1");
const QString kKeyFloatCornerRadiusFmt =
    QStringLiteral("floating/cornerRadius/%1");
const QString kKeyFloatCornerSmoothingFmt =
    QStringLiteral("floating/cornerSmoothing/%1");
const QString kKeyFloatCustomAppearanceFmt =
    QStringLiteral("floating/customAppearance/%1");

// 全局主题。颜色、窗口透明度和浮窗默认外观都放在 theme/ 下，便于一眼看出它们不是每盒配置。
const QString kKeyThemeWindowBg      = QStringLiteral("theme/windowBackground");
const QString kKeyThemeSurface       = QStringLiteral("theme/surface");
const QString kKeyThemeTitleBar      = QStringLiteral("theme/titleBar");
const QString kKeyThemeText          = QStringLiteral("theme/text");
const QString kKeyThemeMutedText     = QStringLiteral("theme/mutedText");
const QString kKeyThemeBorder        = QStringLiteral("theme/border");
const QString kKeyThemeHover         = QStringLiteral("theme/hover");
const QString kKeyThemePressed       = QStringLiteral("theme/pressed");
const QString kKeyThemePrimary       = QStringLiteral("theme/primary");
const QString kKeyThemePrimaryHover  = QStringLiteral("theme/primaryHover");
const QString kKeyThemePrimaryPress  = QStringLiteral("theme/primaryPressed");
const QString kKeyThemeOnPrimary     = QStringLiteral("theme/onPrimary");
const QString kKeyThemeDanger        = QStringLiteral("theme/danger");
const QString kKeyThemeDangerPress   = QStringLiteral("theme/dangerPressed");
const QString kKeyThemeWindowOpacity = QStringLiteral("theme/windowOpacity");

const QString kKeyThemeFloatViewModeFmt = QStringLiteral("theme/float/viewMode");
const QString kKeyThemeFloatIconSizeFmt = QStringLiteral("theme/float/iconSize");
const QString kKeyThemeFloatOpacityFmt = QStringLiteral("theme/float/opacity");
const QString kKeyThemeFloatHoverEffectFmt = QStringLiteral("theme/float/hoverEffect");
const QString kKeyThemeFloatHoverStrengthFmt = QStringLiteral("theme/float/hoverStrength");
const QString kKeyThemeFloatAnimationSpeedFmt = QStringLiteral("theme/float/animationSpeed");
const QString kKeyThemeFloatHoverExpandDelayFmt = QStringLiteral("theme/float/hoverExpandDelay");
const QString kKeyThemeFloatHoverCollapseDelayFmt = QStringLiteral("theme/float/hoverCollapseDelay");
const QString kKeyThemeFloatCornerRadiusFmt = QStringLiteral("theme/float/cornerRadius");
const QString kKeyThemeFloatCornerSmoothingFmt = QStringLiteral("theme/float/cornerSmoothing");

// 把盒名编码成可安全嵌入 QSettings 键名的形式。
//
// 为什么需要：QSettings 用 '/' 作层级分隔符，盒名里若出现 '/' 会被解释成
// 多一层目录，导致写入与读取落在不同的键上。
// CoreNames::sanitizeBoxName 已经挡掉了 '\' '/' 等非法字符，但那是 UI 侧
// 的输入清洗，属于"上游约定"；配置层不该依赖上游永远正确 —— 一旦有人
// 从别的路径创建了盒目录（比如主人在资源管理器里手工建），这里就会炸。
//
// 用 Base64Url 而非普通 Base64：编码结果不含 '/' 与 '+'，
// 既不会与 QSettings 分隔符冲突，也不需要在 ini 里做转义。
// 同时省掉结尾的 '='，让键名短一点、看着干净。
QString encodeBoxName(const QString &boxName)
{
    return QString::fromLatin1(
        boxName.toUtf8().toBase64(QByteArray::Base64UrlEncoding
                                  | QByteArray::OmitTrailingEquals));
}

// 解析 ini 全路径并按需建父目录。
// 返回空串表示环境异常（AppConfigLocation 拿不到），此时所有读写退化为默认值。
QString resolveIniPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        return QString();

    // AppConfigLocation 在 Qt6 下已包含组织名/应用名两级，通常就是
    // ...\DeskTidy\DeskTidy。这里只保证目录存在，不再追加多余层级。
    QDir().mkpath(dir);
    return QDir::cleanPath(dir + QStringLiteral("/DeskTidy.ini"));
}

} // namespace

Settings::Settings()
{
    m_iniPath = resolveIniPath();
}

QStringList Settings::excludedNames() const
{
    if (m_iniPath.isEmpty())
        return QStringList();

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QStringList raw = ini.value(kKeyExcluded).toStringList();

    // 顺手清洗：去空白、丢空串。手工编辑过 ini 的主人常留下 "a, ,b" 这类残留。
    QStringList cleaned;
    cleaned.reserve(raw.size());
    for (const QString &name : raw) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty())
            cleaned.append(trimmed);
    }
    return cleaned;
}

void Settings::setExcludedNames(const QStringList &names)
{
    if (m_iniPath.isEmpty())
        return;

    QStringList cleaned;
    cleaned.reserve(names.size());
    for (const QString &name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty())
            cleaned.append(trimmed);
    }

    QSettings ini(m_iniPath, QSettings::IniFormat);
    ini.setValue(kKeyExcluded, cleaned);
    ini.sync();     // 立即落盘：本工具不常驻，退出前不保证还有 sync 机会
}

QString Settings::lastBoxName() const
{
    if (m_iniPath.isEmpty())
        return QString();

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyLastBox).toString();
}

void Settings::setLastBoxName(const QString &name)
{
    if (m_iniPath.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    if (name.isEmpty())
        ini.remove(kKeyLastBox);    // 空值不留脏键
    else
        ini.setValue(kKeyLastBox, name);
    ini.sync();
}

// ---------------------------------------------------------------------------
// 常驻浮窗相关
// ---------------------------------------------------------------------------

QStringList Settings::openBoxNames() const
{
    if (m_iniPath.isEmpty())
        return QStringList();

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QStringList raw = ini.value(kKeyFloatBoxes).toStringList();

    // 与 excludedNames 同样的清洗口径：去空白、丢空串。
    QStringList cleaned;
    cleaned.reserve(raw.size());
    for (const QString &name : raw) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty())
            cleaned.append(trimmed);
    }
    return cleaned;
}

void Settings::setOpenBoxNames(const QStringList &names)
{
    if (m_iniPath.isEmpty())
        return;

    QStringList cleaned;
    cleaned.reserve(names.size());
    for (const QString &name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed))
            cleaned.append(trimmed);      // 顺手去重：重复项没有任何意义
    }

    QSettings ini(m_iniPath, QSettings::IniFormat);
    if (cleaned.isEmpty())
        ini.remove(kKeyFloatBoxes);       // 空值不留脏键
    else
        ini.setValue(kKeyFloatBoxes, cleaned);
    ini.sync();
}

QByteArray Settings::floatGeometry(const QString &boxName) const
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return QByteArray();

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyFloatGeomFmt.arg(encodeBoxName(boxName))).toByteArray();
}

void Settings::setFloatGeometry(const QString &boxName, const QByteArray &blob)
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString key = kKeyFloatGeomFmt.arg(encodeBoxName(boxName));

    // 浮窗关掉时几何**保留**（下次开还在原位），所以这里只有"存"没有"删"。
    // 空 blob 视为"清除"，供日后需要时使用。
    if (blob.isEmpty())
        ini.remove(key);
    else
        ini.setValue(key, blob);
    ini.sync();
}

bool Settings::floatRolledUp(const QString &boxName) const
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return false;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyFloatRollFmt.arg(encodeBoxName(boxName)), false).toBool();
}

void Settings::setFloatRolledUp(const QString &boxName, bool rolledUp)
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString key = kKeyFloatRollFmt.arg(encodeBoxName(boxName));

    // 展开是默认状态，不必落键 —— 少一个键就少一份可能失真的状态。
    if (rolledUp)
        ini.setValue(key, true);
    else
        ini.remove(key);
    ini.sync();
}

// ---------------------------------------------------------------------------
// 浮窗外观（每盒一份）
//
// 三项都沿用 floatRolledUp 那套"只存非默认值"的约定：
// 默认外观不落任何键 —— 少一个键就少一份可能失真的状态，
// 也免得配置文件为每个盒留一堆恒等于默认值的冗余行。
// ---------------------------------------------------------------------------
BoxAppearance Settings::floatAppearance(const QString &boxName) const
{
    BoxAppearance result;       // 默认构造即"列表 + 不透明"，与改造前一致

    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return result;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString encoded = encodeBoxName(boxName);

    // 视图模式：越界或读不到都退回 List。
    // 手工编辑过 ini（或从别的版本迁来）时可能存着非法值，
    // 这里必须夹紧，否则会拿一个不存在的枚举去 switch。
    const int mode = ini.value(kKeyFloatViewModeFmt.arg(encoded),
                               static_cast<int>(BoxAppearance::ViewMode::List)).toInt();
    switch (mode) {
    case static_cast<int>(BoxAppearance::ViewMode::SmallIcon):
        result.viewMode = BoxAppearance::ViewMode::SmallIcon;
        break;
    case static_cast<int>(BoxAppearance::ViewMode::MediumIcon):
        result.viewMode = BoxAppearance::ViewMode::MediumIcon;
        break;
    case static_cast<int>(BoxAppearance::ViewMode::LargeIcon):
        result.viewMode = BoxAppearance::ViewMode::LargeIcon;
        break;
    default:
        result.viewMode = BoxAppearance::ViewMode::List;
        break;
    }

    // 图标尺寸：0（或负数）表示"跟随视图模式"。
    const int px = ini.value(kKeyFloatIconSizeFmt.arg(encoded), 0).toInt();
    result.iconSize = px > 0 ? px : 0;

    // 透明度：夹在 [kMinOpacity, 100]。
    //
    // 下限不是 1 而是 kMinOpacity（20）：完全透明 = 浮窗彻底看不见，
    // 而主人没有入口把它调回来，属于不可恢复的故障。宁可显得保守。
    const int op = ini.value(kKeyFloatOpacityFmt.arg(encoded), 100).toInt();
    result.opacity = qBound(BoxAppearance::kMinOpacity, op, 100);

    // 悬停触感与动画：旧配置里根本没有这几个键，读不到就走默认值；
    // 手工改成非法枚举 / 越界数值时归一化到最近的预设档。
    result.hoverEffect = BoxAppearance::normalizeHoverEffect(
        ini.value(kKeyFloatHoverEffectFmt.arg(encoded),
                  static_cast<int>(BoxAppearance::HoverEffect::Glow)).toInt());
    result.feedbackStrength = BoxAppearance::normalizeFeedbackStrength(
        ini.value(kKeyFloatHoverStrengthFmt.arg(encoded),
                  static_cast<int>(BoxAppearance::FeedbackStrength::Standard)).toInt());
    result.animationSpeed = BoxAppearance::normalizeAnimationSpeed(
        ini.value(kKeyFloatAnimationSpeedFmt.arg(encoded),
                  static_cast<int>(BoxAppearance::AnimationSpeed::Standard)).toInt());
    result.hoverExpandDelayMs = BoxAppearance::normalizeHoverExpandDelayMs(
        ini.value(kKeyFloatHoverExpandDelayFmt.arg(encoded), 250).toInt());
    result.hoverCollapseDelayMs = BoxAppearance::normalizeHoverCollapseDelayMs(
        ini.value(kKeyFloatHoverCollapseDelayFmt.arg(encoded), 400).toInt());
    result.cornerRadius = BoxAppearance::normalizeCornerRadius(
        ini.value(kKeyFloatCornerRadiusFmt.arg(encoded), 8).toInt());
    result.cornerSmoothing = BoxAppearance::normalizeCornerSmoothing(
        ini.value(kKeyFloatCornerSmoothingFmt.arg(encoded), 1).toInt());

    return result;
}

void Settings::setFloatAppearance(const QString &boxName, const BoxAppearance &appearance)
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString encoded = encodeBoxName(boxName);

    // 视图模式：List 是默认，不落键。
    const QString viewKey = kKeyFloatViewModeFmt.arg(encoded);
    if (appearance.viewMode == BoxAppearance::ViewMode::List)
        ini.remove(viewKey);
    else
        ini.setValue(viewKey, static_cast<int>(appearance.viewMode));

    // 图标尺寸：0（跟随视图模式）是默认，不落键。
    const QString sizeKey = kKeyFloatIconSizeFmt.arg(encoded);
    if (appearance.iconSize > 0)
        ini.setValue(sizeKey, appearance.iconSize);
    else
        ini.remove(sizeKey);

    // 透明度：100 是默认，不落键。
    // 低于 kMinOpacity 的输入夹到下限而不是当成"重置为默认" ——
    // 调用方传 0 多半是滑块没初始化好，静默改成 100 会让人莫名其妙。
    const QString opKey = kKeyFloatOpacityFmt.arg(encoded);
    const int clamped = qBound(BoxAppearance::kMinOpacity, appearance.opacity, 100);
    if (clamped >= 100)
        ini.remove(opKey);
    else
        ini.setValue(opKey, clamped);

    // ---- 悬停触感与动画（同样只存非默认值） ------------------------------
    // 写入前统一归一化：调用方传进来的越界值不应该被原样落到 ini 里，
    // 否则下次读取还得再夹一次，而且配置文件里的值会一直看着不对劲。
    const auto writeEnumKey = [&ini](const QString &key, int value, int defaultValue) {
        if (value == defaultValue)
            ini.remove(key);
        else
            ini.setValue(key, value);
    };

    const QString hoverKey = kKeyFloatHoverEffectFmt.arg(encoded);
    writeEnumKey(hoverKey,
                 static_cast<int>(BoxAppearance::normalizeHoverEffect(
                     static_cast<int>(appearance.hoverEffect))),
                 static_cast<int>(BoxAppearance::HoverEffect::Glow));

    const QString strengthKey = kKeyFloatHoverStrengthFmt.arg(encoded);
    writeEnumKey(strengthKey,
                 static_cast<int>(BoxAppearance::normalizeFeedbackStrength(
                     static_cast<int>(appearance.feedbackStrength))),
                 static_cast<int>(BoxAppearance::FeedbackStrength::Standard));

    const QString speedKey = kKeyFloatAnimationSpeedFmt.arg(encoded);
    writeEnumKey(speedKey,
                 static_cast<int>(BoxAppearance::normalizeAnimationSpeed(
                     static_cast<int>(appearance.animationSpeed))),
                 static_cast<int>(BoxAppearance::AnimationSpeed::Standard));

    const QString expandKey = kKeyFloatHoverExpandDelayFmt.arg(encoded);
    writeEnumKey(expandKey,
                 BoxAppearance::normalizeHoverExpandDelayMs(appearance.hoverExpandDelayMs),
                 250);

    const QString collapseKey = kKeyFloatHoverCollapseDelayFmt.arg(encoded);
    writeEnumKey(collapseKey,
                 BoxAppearance::normalizeHoverCollapseDelayMs(appearance.hoverCollapseDelayMs),
                 400);

    const QString cornerKey = kKeyFloatCornerRadiusFmt.arg(encoded);
    writeEnumKey(cornerKey,
                 BoxAppearance::normalizeCornerRadius(appearance.cornerRadius),
                 8);

    const QString smoothingKey = kKeyFloatCornerSmoothingFmt.arg(encoded);
    writeEnumKey(smoothingKey,
                 BoxAppearance::normalizeCornerSmoothing(appearance.cornerSmoothing),
                 1);

    // 只要主人从浮窗右键点过「应用」，这个盒就拥有一份完整覆盖。
    // 即使这份覆盖恰好等于内置默认值，也不能被全局主题继续带着走。
    ini.setValue(kKeyFloatCustomAppearanceFmt.arg(encoded), true);
    ini.sync();
}

void Settings::clearFloatAppearanceOverride(const QString &boxName)
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString encoded = encodeBoxName(boxName);

    ini.remove(kKeyFloatViewModeFmt.arg(encoded));
    ini.remove(kKeyFloatIconSizeFmt.arg(encoded));
    ini.remove(kKeyFloatOpacityFmt.arg(encoded));
    ini.remove(kKeyFloatHoverEffectFmt.arg(encoded));
    ini.remove(kKeyFloatHoverStrengthFmt.arg(encoded));
    ini.remove(kKeyFloatAnimationSpeedFmt.arg(encoded));
    ini.remove(kKeyFloatHoverExpandDelayFmt.arg(encoded));
    ini.remove(kKeyFloatHoverCollapseDelayFmt.arg(encoded));
    ini.remove(kKeyFloatCornerRadiusFmt.arg(encoded));
    ini.remove(kKeyFloatCornerSmoothingFmt.arg(encoded));
    ini.remove(kKeyFloatCustomAppearanceFmt.arg(encoded));
    ini.sync();
}

void Settings::clearFloatAppearance(const QString &boxName)
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return;

    // 只在这里清掉仅属于“删除盒”的钉住状态；恢复跟随全局必须走
    // clearFloatAppearanceOverride()，不能误删主人的位置约束。
    clearFloatAppearanceOverride(boxName);

    QSettings ini(m_iniPath, QSettings::IniFormat);
    ini.remove(kKeyFloatLockedFmt.arg(encodeBoxName(boxName)));
    ini.sync();
}

bool Settings::hasFloatAppearanceOverride(const QString &boxName) const
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return false;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString encoded = encodeBoxName(boxName);
    return ini.value(kKeyFloatCustomAppearanceFmt.arg(encoded), false).toBool()
           || ini.contains(kKeyFloatViewModeFmt.arg(encoded))
           || ini.contains(kKeyFloatIconSizeFmt.arg(encoded))
           || ini.contains(kKeyFloatOpacityFmt.arg(encoded))
           || ini.contains(kKeyFloatHoverEffectFmt.arg(encoded))
           || ini.contains(kKeyFloatHoverStrengthFmt.arg(encoded))
           || ini.contains(kKeyFloatAnimationSpeedFmt.arg(encoded))
           || ini.contains(kKeyFloatHoverExpandDelayFmt.arg(encoded))
           || ini.contains(kKeyFloatHoverCollapseDelayFmt.arg(encoded))
           || ini.contains(kKeyFloatCornerRadiusFmt.arg(encoded))
           || ini.contains(kKeyFloatCornerSmoothingFmt.arg(encoded));
}

// ---------------------------------------------------------------------------
// 全局主题
// ---------------------------------------------------------------------------
AppTheme Settings::appTheme() const
{
    AppTheme result;
    if (m_iniPath.isEmpty())
        return result;

    QSettings ini(m_iniPath, QSettings::IniFormat);

    const auto readColor = [&ini](const QString &key, const QColor &fallback) {
        const QString raw = ini.value(key, fallback.name(QColor::HexArgb)).toString();
        const QColor color = QColor::fromString(raw);
        return color.isValid() ? color : fallback;
    };

    result.windowBackground = readColor(kKeyThemeWindowBg, result.windowBackground);
    result.surface          = readColor(kKeyThemeSurface, result.surface);
    result.titleBar         = readColor(kKeyThemeTitleBar, result.titleBar);
    result.text             = readColor(kKeyThemeText, result.text);
    result.mutedText        = readColor(kKeyThemeMutedText, result.mutedText);
    result.border           = readColor(kKeyThemeBorder, result.border);
    result.hover            = readColor(kKeyThemeHover, result.hover);
    result.pressed          = readColor(kKeyThemePressed, result.pressed);
    result.primary          = readColor(kKeyThemePrimary, result.primary);
    result.primaryHover     = readColor(kKeyThemePrimaryHover, result.primaryHover);
    result.primaryPressed   = readColor(kKeyThemePrimaryPress, result.primaryPressed);
    result.onPrimary        = readColor(kKeyThemeOnPrimary, result.onPrimary);
    result.danger           = readColor(kKeyThemeDanger, result.danger);
    result.dangerPressed    = readColor(kKeyThemeDangerPress, result.dangerPressed);
    result.windowOpacity = std::max(
        40, std::min(ini.value(kKeyThemeWindowOpacity, result.windowOpacity).toInt(), 100));

    BoxAppearance &a = result.floatDefaults;
    a.viewMode = static_cast<BoxAppearance::ViewMode>(
        std::max(0, std::min(ini.value(kKeyThemeFloatViewModeFmt,
                                       static_cast<int>(a.viewMode)).toInt(), 3)));
    a.iconSize = std::max(0, std::min(ini.value(kKeyThemeFloatIconSizeFmt, a.iconSize).toInt(), 512));
    a.opacity = std::max(BoxAppearance::kMinOpacity,
                         std::min(ini.value(kKeyThemeFloatOpacityFmt, a.opacity).toInt(), 100));
    a.hoverEffect = BoxAppearance::normalizeHoverEffect(
        ini.value(kKeyThemeFloatHoverEffectFmt,
                  static_cast<int>(a.hoverEffect)).toInt());
    a.feedbackStrength = BoxAppearance::normalizeFeedbackStrength(
        ini.value(kKeyThemeFloatHoverStrengthFmt,
                  static_cast<int>(a.feedbackStrength)).toInt());
    a.animationSpeed = BoxAppearance::normalizeAnimationSpeed(
        ini.value(kKeyThemeFloatAnimationSpeedFmt,
                  static_cast<int>(a.animationSpeed)).toInt());
    a.hoverExpandDelayMs = BoxAppearance::normalizeHoverExpandDelayMs(
        ini.value(kKeyThemeFloatHoverExpandDelayFmt, a.hoverExpandDelayMs).toInt());
    a.hoverCollapseDelayMs = BoxAppearance::normalizeHoverCollapseDelayMs(
        ini.value(kKeyThemeFloatHoverCollapseDelayFmt, a.hoverCollapseDelayMs).toInt());
    a.cornerRadius = BoxAppearance::normalizeCornerRadius(
        ini.value(kKeyThemeFloatCornerRadiusFmt, a.cornerRadius).toInt());
    a.cornerSmoothing = BoxAppearance::normalizeCornerSmoothing(
        ini.value(kKeyThemeFloatCornerSmoothingFmt, a.cornerSmoothing).toInt());

    result.normalize();
    return result;
}

void Settings::setAppTheme(const AppTheme &theme)
{
    if (m_iniPath.isEmpty())
        return;

    AppTheme value = theme;
    value.normalize();
    const AppTheme d;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const auto writeColor = [&ini](const QString &key, const QColor &color, const QColor &fallback) {
        if (color == fallback)
            ini.remove(key);
        else
            ini.setValue(key, color.name(QColor::HexArgb));
    };

    writeColor(kKeyThemeWindowBg, value.windowBackground, d.windowBackground);
    writeColor(kKeyThemeSurface, value.surface, d.surface);
    writeColor(kKeyThemeTitleBar, value.titleBar, d.titleBar);
    writeColor(kKeyThemeText, value.text, d.text);
    writeColor(kKeyThemeMutedText, value.mutedText, d.mutedText);
    writeColor(kKeyThemeBorder, value.border, d.border);
    writeColor(kKeyThemeHover, value.hover, d.hover);
    writeColor(kKeyThemePressed, value.pressed, d.pressed);
    writeColor(kKeyThemePrimary, value.primary, d.primary);
    writeColor(kKeyThemePrimaryHover, value.primaryHover, d.primaryHover);
    writeColor(kKeyThemePrimaryPress, value.primaryPressed, d.primaryPressed);
    writeColor(kKeyThemeOnPrimary, value.onPrimary, d.onPrimary);
    writeColor(kKeyThemeDanger, value.danger, d.danger);
    writeColor(kKeyThemeDangerPress, value.dangerPressed, d.dangerPressed);

    if (value.windowOpacity == d.windowOpacity)
        ini.remove(kKeyThemeWindowOpacity);
    else
        ini.setValue(kKeyThemeWindowOpacity, value.windowOpacity);

    const BoxAppearance &a = value.floatDefaults;
    const BoxAppearance &fd = d.floatDefaults;
    const auto writeInt = [&ini](const QString &key, int current, int fallback) {
        if (current == fallback)
            ini.remove(key);
        else
            ini.setValue(key, current);
    };

    writeInt(kKeyThemeFloatViewModeFmt, static_cast<int>(a.viewMode),
             static_cast<int>(fd.viewMode));
    writeInt(kKeyThemeFloatIconSizeFmt, a.iconSize, fd.iconSize);
    writeInt(kKeyThemeFloatOpacityFmt, a.opacity, fd.opacity);
    writeInt(kKeyThemeFloatHoverEffectFmt, static_cast<int>(a.hoverEffect),
             static_cast<int>(fd.hoverEffect));
    writeInt(kKeyThemeFloatHoverStrengthFmt, static_cast<int>(a.feedbackStrength),
             static_cast<int>(fd.feedbackStrength));
    writeInt(kKeyThemeFloatAnimationSpeedFmt, static_cast<int>(a.animationSpeed),
             static_cast<int>(fd.animationSpeed));
    writeInt(kKeyThemeFloatHoverExpandDelayFmt, a.hoverExpandDelayMs,
             fd.hoverExpandDelayMs);
    writeInt(kKeyThemeFloatHoverCollapseDelayFmt, a.hoverCollapseDelayMs,
             fd.hoverCollapseDelayMs);
    writeInt(kKeyThemeFloatCornerRadiusFmt, a.cornerRadius, fd.cornerRadius);
    writeInt(kKeyThemeFloatCornerSmoothingFmt, a.cornerSmoothing, fd.cornerSmoothing);

    ini.sync();
}

// ---------------------------------------------------------------------------
// 浮窗"钉住"状态（每盒一份）
//
// 沿用 floatRolledUp 那套"只存非默认值"的约定：默认（没钉住）不落键。
// 默认值是"不钉住"，因为钉住是一个主人主动施加的约束 ——
// 不落键就永远不会出现"我明明没锁过，它却是锁着的"。
// ---------------------------------------------------------------------------

bool Settings::floatLocked(const QString &boxName) const
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return false;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyFloatLockedFmt.arg(encodeBoxName(boxName)), false).toBool();
}

void Settings::setFloatLocked(const QString &boxName, bool locked)
{
    if (m_iniPath.isEmpty() || boxName.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    const QString key = kKeyFloatLockedFmt.arg(encodeBoxName(boxName));

    // 没锁是默认状态，不落键。
    if (locked)
        ini.setValue(key, true);
    else
        ini.remove(key);
    ini.sync();
}

bool Settings::alwaysOnTop() const
{
    if (m_iniPath.isEmpty())
        return true;    // 默认置顶：这是浮窗的预期行为

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyAlwaysOnTop, true).toBool();
}

void Settings::setAlwaysOnTop(bool on)
{
    if (m_iniPath.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    ini.setValue(kKeyAlwaysOnTop, on);
    ini.sync();
}

bool Settings::trayHintShown() const
{
    if (m_iniPath.isEmpty())
        return false;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyTrayHint, false).toBool();
}

void Settings::setTrayHintShown(bool shown)
{
    if (m_iniPath.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    if (shown)
        ini.setValue(kKeyTrayHint, true);
    else
        ini.remove(kKeyTrayHint);   // 空值不留脏键
    ini.sync();
}

// ---------------------------------------------------------------------------
// 界面动画总开关
// ---------------------------------------------------------------------------
bool Settings::animationsEnabled() const
{
    if (m_iniPath.isEmpty())
        return true;    // 配置不可用时按"开启"处理：动画是默认体验

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyAnimations, true).toBool();
}

void Settings::setAnimationsEnabled(bool on)
{
    if (m_iniPath.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);

    // ⚠️ 这里**不**用 setTrayHintShown 那套"false 就 remove"的写法。
    // 那一套是给"只能为真才算有意义"的一次性标记用的（提示展示过没有）；
    // 本项 true/false 都是主人可能明确选择的正常状态，
    // 若把 false 写成"删键"，配置里就看不出"主人关过"与"从没设过"的区别 ——
    // 而排查"为什么我这没有动画"时，这个区别正是第一时间要看的东西。
    ini.setValue(kKeyAnimations, on);
    ini.sync();
}

// ---------------------------------------------------------------------------
// 悬停自动展开总开关
// ---------------------------------------------------------------------------
bool Settings::hoverExpandEnabled() const
{
    if (m_iniPath.isEmpty())
        return true;    // 配置不可用时按"开启"处理，同 animationsEnabled

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyHoverExpand, true).toBool();
}

void Settings::setHoverExpandEnabled(bool on)
{
    if (m_iniPath.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    // 与 setAnimationsEnabled 同理：true/false 都是主人可能明确选的状态，
    // 不用"false 就 remove"那套。否则日后主人报"我这浮窗怎么不自动展开"，
    // 翻配置分不清是他关过还是从没设过。
    ini.setValue(kKeyHoverExpand, on);
    ini.sync();
}

// ---------------------------------------------------------------------------
// 开机自启方式偏好
// ---------------------------------------------------------------------------
bool Settings::autoStartSilent() const
{
    if (m_iniPath.isEmpty())
        return false;   // 默认普通自启

    QSettings ini(m_iniPath, QSettings::IniFormat);
    return ini.value(kKeyAutoStartSilent, false).toBool();
}

void Settings::setAutoStartSilent(bool silent)
{
    if (m_iniPath.isEmpty())
        return;

    QSettings ini(m_iniPath, QSettings::IniFormat);
    // 和动画/悬停开关一样，true、false 都是明确选择，不能让"关过"与
    // "从没设过"在配置里无法区分。
    ini.setValue(kKeyAutoStartSilent, silent);
    ini.sync();
}
