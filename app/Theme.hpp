#pragma once

#include <QColor>
#include <QPalette>
#include <QString>
#include <QStringList>

namespace zproject::app {

// Design tokens for the whole window (zwriter's ThemeColors pattern, copied
// per SPEC.md decision 32): one palette per theme, one application style
// sheet built from it. Accent #2fa8f0 (SPEC.md §23) for focus rings, borders
// and selection; text on it uses accentText, which keeps 4.5:1 (SPEC.md §18).
struct ThemeColors {
  QString id;          // "light", "dark" or "high-contrast" (never "system")
  QColor windowBg;
  QColor barBg;        // menu bar and status bar
  QColor panelBg;      // sidebar and inspector
  QColor inputBg;      // fields, buttons, lists
  QColor fg;
  QColor muted;        // secondary text
  QColor border;
  QColor hover;
  QColor pressed;
  QColor accent;       // #2fa8f0 (high contrast: a brighter tint)
  QColor accentText;   // links and accent-colored text
  QColor selBg;
  QColor selFg;
  QColor bannerBg;     // offline banner
  QColor bannerFg;
  QColor bannerBorder;
};

namespace Theme {

inline constexpr const char* kAccent = "#2fa8f0";

// The theme choices in Preferences and View → Theme.
QStringList choices();          // {"system", "light", "dark", "high-contrast"}
QString displayName(const QString& choice);

// "system" follows the OS: Qt 6.5+ asks QStyleHints::colorScheme(); on Qt
// 6.4 (the Linux desktops) the palette's window lightness decides.
QString resolve(const QString& choice, const QPalette& systemPalette);
bool paletteIsDark(const QPalette& p);

ThemeColors colors(const QString& themeId);
// A QPalette matching the tokens, for widgets the style sheet doesn't reach.
QPalette palette(const ThemeColors& c);
// The application style sheet; bodyPt is the zoomed UI text size.
QString styleSheet(const ThemeColors& c, double bodyPt);

// WCAG 2 contrast ratio of two opaque colors (1..21).
double contrastRatio(const QColor& a, const QColor& b);

}  // namespace Theme

// UI text zoom (View → Zoom; Ctrl/Cmd + = / − / 0), zwriter's ViewZoom steps.
namespace ViewZoom {
inline constexpr int kMinPercent = 50;
inline constexpr int kMaxPercent = 300;
inline constexpr int kDefaultPercent = 100;
inline constexpr double kBasePt = 10.0;  // UI text at 100 %
int clampPercent(int p);
int stepUp(int p);
int stepDown(int p);
double pointSize(int percent);
}  // namespace ViewZoom

}  // namespace zproject::app
