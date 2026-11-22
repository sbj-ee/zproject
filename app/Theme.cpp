#include "app/Theme.hpp"

#include <QGuiApplication>
#include <QStyleHints>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>

namespace zproject::app {

namespace Theme {

QStringList choices() {
  return {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("high-contrast")};
}

QString displayName(const QString& choice) {
  if (choice == QLatin1String("light")) return QObject::tr("Light");
  if (choice == QLatin1String("dark")) return QObject::tr("Dark");
  if (choice == QLatin1String("high-contrast")) return QObject::tr("High contrast");
  return QObject::tr("System");
}

bool paletteIsDark(const QPalette& p) {
  return p.color(QPalette::Window).lightness() < p.color(QPalette::WindowText).lightness();
}

QString resolve(const QString& choice, const QPalette& systemPalette) {
  if (choice == QLatin1String("light") || choice == QLatin1String("dark") ||
      choice == QLatin1String("high-contrast")) {
    return choice;
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  if (const QStyleHints* hints = QGuiApplication::styleHints()) {
    if (hints->colorScheme() == Qt::ColorScheme::Dark) return QStringLiteral("dark");
    if (hints->colorScheme() == Qt::ColorScheme::Light) return QStringLiteral("light");
  }
#endif
  // Qt 6.4 (and an unknown scheme): the platform palette decides.
  return paletteIsDark(systemPalette) ? QStringLiteral("dark") : QStringLiteral("light");
}

ThemeColors colors(const QString& themeId) {
  ThemeColors c;
  if (themeId == QLatin1String("dark")) {
    c.id = QStringLiteral("dark");
    c.windowBg = QColor(0x1b, 0x1d, 0x21);
    c.barBg = QColor(0x23, 0x26, 0x2b);
    c.panelBg = QColor(0x1f, 0x22, 0x26);
    c.inputBg = QColor(0x16, 0x18, 0x1b);
    c.fg = QColor(0xe2, 0xe4, 0xe8);
    c.muted = QColor(0x9a, 0xa1, 0xab);
    c.border = QColor(0x36, 0x3a, 0x41);
    c.hover = QColor(0x2c, 0x30, 0x36);
    c.pressed = QColor(0x36, 0x3b, 0x42);
    c.accent = QColor(QLatin1String(kAccent));
    c.accentText = QColor(0x6c, 0xc4, 0xf6);
    c.selBg = QColor(0x1d, 0x5d, 0x86);
    c.selFg = QColor(0xff, 0xff, 0xff);
    c.bannerBg = QColor(0x3d, 0x32, 0x12);
    c.bannerFg = QColor(0xf6, 0xe7, 0xb8);
    c.bannerBorder = QColor(0x6b, 0x56, 0x1c);
  } else if (themeId == QLatin1String("high-contrast")) {
    c.id = QStringLiteral("high-contrast");
    c.windowBg = QColor(0x00, 0x00, 0x00);
    c.barBg = QColor(0x00, 0x00, 0x00);
    c.panelBg = QColor(0x00, 0x00, 0x00);
    c.inputBg = QColor(0x00, 0x00, 0x00);
    c.fg = QColor(0xff, 0xff, 0xff);
    c.muted = QColor(0xe0, 0xe0, 0xe0);
    c.border = QColor(0xff, 0xff, 0xff);
    c.hover = QColor(0x26, 0x26, 0x26);
    c.pressed = QColor(0x40, 0x40, 0x40);
    c.accent = QColor(0x5c, 0xc3, 0xff);
    c.accentText = QColor(0x8f, 0xd6, 0xff);
    c.selBg = QColor(0xff, 0xff, 0x00);
    c.selFg = QColor(0x00, 0x00, 0x00);
    c.bannerBg = QColor(0x00, 0x00, 0x00);
    c.bannerFg = QColor(0xff, 0xff, 0x00);
    c.bannerBorder = QColor(0xff, 0xff, 0x00);
  } else {
    c.id = QStringLiteral("light");
    c.windowBg = QColor(0xf4, 0xf6, 0xf8);
    c.barBg = QColor(0xfa, 0xfb, 0xfc);
    c.panelBg = QColor(0xee, 0xf1, 0xf4);
    c.inputBg = QColor(0xff, 0xff, 0xff);
    c.fg = QColor(0x1d, 0x23, 0x2b);
    c.muted = QColor(0x5b, 0x65, 0x72);
    c.border = QColor(0xd5, 0xdb, 0xe2);
    c.hover = QColor(0xe4, 0xe9, 0xee);
    c.pressed = QColor(0xd6, 0xdd, 0xe5);
    c.accent = QColor(QLatin1String(kAccent));
    c.accentText = QColor(0x0b, 0x65, 0x9e);
    c.selBg = QColor(0xcf, 0xe9, 0xfb);
    c.selFg = QColor(0x0b, 0x2a, 0x3f);
    c.bannerBg = QColor(0xfd, 0xf3, 0xd4);
    c.bannerFg = QColor(0x5c, 0x45, 0x00);
    c.bannerBorder = QColor(0xe8, 0xcf, 0x7d);
  }
  return c;
}

QPalette palette(const ThemeColors& c) {
  QPalette p;
  p.setColor(QPalette::Window, c.windowBg);
  p.setColor(QPalette::WindowText, c.fg);
  p.setColor(QPalette::Base, c.inputBg);
  p.setColor(QPalette::AlternateBase, c.panelBg);
  p.setColor(QPalette::Text, c.fg);
  p.setColor(QPalette::Button, c.inputBg);
  p.setColor(QPalette::ButtonText, c.fg);
  p.setColor(QPalette::ToolTipBase, c.barBg);
  p.setColor(QPalette::ToolTipText, c.fg);
  p.setColor(QPalette::PlaceholderText, c.muted);
  p.setColor(QPalette::Highlight, c.selBg);
  p.setColor(QPalette::HighlightedText, c.selFg);
  p.setColor(QPalette::Link, c.accentText);
  p.setColor(QPalette::Disabled, QPalette::Text, c.muted);
  p.setColor(QPalette::Disabled, QPalette::WindowText, c.muted);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
  return p;
}

QString styleSheet(const ThemeColors& c, double bodyPt) {
  QString css = QStringLiteral(R"CSS(
* { font-size: @bodyPt; }
QMainWindow, QDialog { background-color: @windowBg; color: @fg; }
QLabel { color: @fg; }
QMenuBar { background-color: @barBg; color: @fg; border-bottom: 1px solid @border; padding: 2px 6px; }
QMenuBar::item { padding: 4px 10px; border-radius: 4px; background: transparent; }
QMenuBar::item:selected { background-color: @hover; }
QMenu { background-color: @inputBg; color: @fg; border: 1px solid @border; padding: 4px 0; }
QMenu::item { padding: 5px 28px 5px 24px; }
QMenu::item:selected { background-color: @selBg; color: @selFg; }
QMenu::item:disabled { color: @muted; }
QMenu::separator { height: 1px; background: @border; margin: 4px 10px; }
QStatusBar { background-color: @barBg; color: @muted; border-top: 1px solid @border; }
QStatusBar QLabel { color: @muted; padding: 1px 8px; }
QStatusBar QLabel#connectionStatus { color: @fg; }
QDockWidget { color: @fg; }
QDockWidget::title { background-color: @barBg; padding: 4px 8px; border-bottom: 1px solid @border; }
#sidebar, #inspector, QListView, QTreeView {
  background-color: @panelBg; color: @fg; border: none;
  selection-background-color: @selBg; selection-color: @selFg;
}
QLineEdit, QTextBrowser, QPlainTextEdit {
  background-color: @inputBg; color: @fg; border: 1px solid @border; border-radius: 4px; padding: 3px 6px;
  selection-background-color: @selBg; selection-color: @selFg;
}
QLineEdit:focus, QTextBrowser:focus, QPushButton:focus { border: 2px solid @accent; }
#sidebar::item { padding: 4px 10px; }
#sidebar:focus, QTableView:focus { border: 2px solid @accent; }
QPushButton {
  color: @fg; background-color: @inputBg; border: 1px solid @border; border-radius: 5px; padding: 5px 14px;
}
QPushButton:hover { background-color: @hover; }
QPushButton:pressed { background-color: @pressed; }
QPushButton:default { border-color: @accent; }
QPushButton:disabled { color: @muted; }
QCheckBox, QRadioButton { color: @fg; }
QGroupBox { color: @fg; border: 1px solid @border; border-radius: 5px; margin-top: 12px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
QTabWidget::pane { border: 1px solid @border; }
QTabBar::tab { color: @fg; background: @barBg; padding: 5px 12px; border: 1px solid @border; }
QTabBar::tab:selected { background: @inputBg; border-bottom-color: @accent; }
QHeaderView::section { background-color: @barBg; color: @fg; border: none; border-bottom: 1px solid @border; padding: 4px 6px; }
QTableView { background-color: @inputBg; color: @fg; gridline-color: @border; }
QToolTip { background-color: @barBg; color: @fg; border: 1px solid @border; padding: 3px 6px; }
#offlineBanner { background-color: @bannerBg; border-bottom: 1px solid @bannerBorder; }
#offlineBanner QLabel { color: @bannerFg; }
#offlineBanner QPushButton { color: @bannerFg; background-color: transparent; border: 1px solid @bannerBorder; }
#emptyState { color: @muted; }
#projectTitle { font-size: @titlePt; font-weight: 600; }
#projectSummary { color: @muted; }
a { color: @accentText; }
)CSS");
  auto hex = [](const QColor& col) { return col.name(QColor::HexRgb); };
  css.replace(QStringLiteral("@titlePt"), QString::number(bodyPt * 1.8, 'f', 1) + QStringLiteral("pt"));
  css.replace(QStringLiteral("@bodyPt"), QString::number(bodyPt, 'f', 1) + QStringLiteral("pt"));
  css.replace(QStringLiteral("@windowBg"), hex(c.windowBg));
  css.replace(QStringLiteral("@barBg"), hex(c.barBg));
  css.replace(QStringLiteral("@panelBg"), hex(c.panelBg));
  css.replace(QStringLiteral("@inputBg"), hex(c.inputBg));
  css.replace(QStringLiteral("@fg"), hex(c.fg));
  css.replace(QStringLiteral("@muted"), hex(c.muted));
  css.replace(QStringLiteral("@border"), hex(c.border));
  css.replace(QStringLiteral("@hover"), hex(c.hover));
  css.replace(QStringLiteral("@pressed"), hex(c.pressed));
  css.replace(QStringLiteral("@accentText"), hex(c.accentText));
  css.replace(QStringLiteral("@accent"), hex(c.accent));
  css.replace(QStringLiteral("@selBg"), hex(c.selBg));
  css.replace(QStringLiteral("@selFg"), hex(c.selFg));
  css.replace(QStringLiteral("@bannerBorder"), hex(c.bannerBorder));
  css.replace(QStringLiteral("@bannerBg"), hex(c.bannerBg));
  css.replace(QStringLiteral("@bannerFg"), hex(c.bannerFg));
  return css;
}

double contrastRatio(const QColor& a, const QColor& b) {
  auto lum = [](const QColor& c) {
    auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
  };
  const double la = lum(a);
  const double lb = lum(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

}  // namespace Theme

namespace ViewZoom {

namespace {
constexpr std::array<int, 12> kSteps{50, 67, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300};
}

int clampPercent(int p) { return qBound(kMinPercent, p, kMaxPercent); }

int stepUp(int p) {
  for (int s : kSteps) {
    if (s > p) return s;
  }
  return kMaxPercent;
}

int stepDown(int p) {
  for (auto it = kSteps.crbegin(); it != kSteps.crend(); ++it) {
    if (*it < p) return *it;
  }
  return kMinPercent;
}

double pointSize(int percent) { return kBasePt * clampPercent(percent) / 100.0; }

}  // namespace ViewZoom

}  // namespace zproject::app
