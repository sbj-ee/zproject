#include "app/Icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <functional>

namespace zproject::app::Icons {
namespace {

// Paints on a 16-unit canvas at 1x and 2x.
QIcon draw(const std::function<void(QPainter&)>& paint) {
  QIcon icon;
  for (int scale : {1, 2}) {
    QPixmap px(16 * scale, 16 * scale);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(scale, scale);
    paint(p);
    p.end();
    icon.addPixmap(px);
  }
  return icon;
}

QPen pen(const QColor& c, double w = 1.4) {
  QPen out(c, w);
  out.setCapStyle(Qt::RoundCap);
  out.setJoinStyle(Qt::RoundJoin);
  return out;
}

QColor ragColor(const QString& rag) {
  if (rag == QLatin1String("green")) return {0x2e, 0x9d, 0x4f};
  if (rag == QLatin1String("amber")) return {0xe0, 0x9b, 0x1a};
  if (rag == QLatin1String("red")) return {0xd2, 0x3b, 0x3b};
  return {};
}

void folder(QPainter& p, const QColor& c) {
  QPainterPath path;
  path.moveTo(1.5, 4);
  path.lineTo(1.5, 13);
  path.lineTo(14.5, 13);
  path.lineTo(14.5, 5.5);
  path.lineTo(7.5, 5.5);
  path.lineTo(6, 3);
  path.lineTo(2.5, 3);
  path.closeSubpath();
  p.setPen(pen(c));
  QColor fill = c;
  fill.setAlphaF(0.15);
  p.setBrush(fill);
  p.drawPath(path);
}

void plus(QPainter& p, const QColor& c, QPointF at, double r) {
  p.setPen(pen(c, 1.6));
  p.drawLine(QPointF(at.x() - r, at.y()), QPointF(at.x() + r, at.y()));
  p.drawLine(QPointF(at.x(), at.y() - r), QPointF(at.x(), at.y() + r));
}

void lines(QPainter& p, const QColor& c) {
  p.setPen(pen(c));
  p.drawLine(QPointF(2, 4), QPointF(14, 4));
  p.drawLine(QPointF(5, 8), QPointF(14, 8));
  p.drawLine(QPointF(5, 12), QPointF(14, 12));
}

void trash(QPainter& p, const QColor& c) {
  p.setPen(pen(c));
  p.setBrush(Qt::NoBrush);
  p.drawLine(QPointF(2.5, 4), QPointF(13.5, 4));
  p.drawLine(QPointF(6, 4), QPointF(6.5, 2));
  p.drawLine(QPointF(6.5, 2), QPointF(9.5, 2));
  p.drawLine(QPointF(9.5, 2), QPointF(10, 4));
  QPainterPath can;
  can.moveTo(4, 4);
  can.lineTo(4.8, 14);
  can.lineTo(11.2, 14);
  can.lineTo(12, 4);
  p.drawPath(can);
  p.drawLine(QPointF(6.8, 6.5), QPointF(6.8, 11.5));
  p.drawLine(QPointF(9.2, 6.5), QPointF(9.2, 11.5));
}

}  // namespace

QIcon glyph(Glyph g, const ThemeColors& c) {
  const QColor fg = c.fg;
  const QColor accent = c.accent;
  return draw([g, fg, accent](QPainter& p) {
    switch (g) {
      case Glyph::NewProject:
        folder(p, fg);
        plus(p, accent, QPointF(8, 9.3), 2.4);
        break;
      case Glyph::Refresh: {
        p.setPen(pen(fg, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(2.5, 2.5, 11, 11), 60 * 16, 270 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        QPainterPath head;
        head.moveTo(10, 1);
        head.lineTo(14, 3.6);
        head.lineTo(9.6, 5.6);
        head.closeSubpath();
        p.drawPath(head);
        break;
      }
      case Glyph::EditProject: {
        p.setPen(pen(fg));
        p.setBrush(Qt::NoBrush);
        QPainterPath pencil;
        pencil.moveTo(3, 13);
        pencil.lineTo(3.6, 10.2);
        pencil.lineTo(11, 2.8);
        pencil.lineTo(13.2, 5);
        pencil.lineTo(5.8, 12.4);
        pencil.closeSubpath();
        p.drawPath(pencil);
        p.drawLine(QPointF(9.6, 4.2), QPointF(11.8, 6.4));
        break;
      }
      case Glyph::DeleteProject:
      case Glyph::DeleteTask:
        trash(p, fg);
        break;
      case Glyph::AllProjects:
        p.setPen(pen(fg));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(2, 2, 5, 5), 1, 1);
        p.drawRoundedRect(QRectF(9, 2, 5, 5), 1, 1);
        p.drawRoundedRect(QRectF(2, 9, 5, 5), 1, 1);
        p.drawRoundedRect(QRectF(9, 9, 5, 5), 1, 1);
        break;
      case Glyph::ProjectDetails:
        folder(p, fg);
        p.setPen(pen(accent, 1.5));
        p.drawLine(QPointF(5, 8.5), QPointF(11, 8.5));
        p.drawLine(QPointF(5, 10.8), QPointF(9, 10.8));
        break;
      case Glyph::Outline:
        lines(p, fg);
        break;
      case Glyph::InsertTask:
        p.setPen(pen(fg));
        p.drawLine(QPointF(2, 4), QPointF(14, 4));
        p.drawLine(QPointF(2, 12), QPointF(8, 12));
        plus(p, accent, QPointF(12, 11), 2.5);
        break;
      case Glyph::TaskDetails:
        p.setPen(pen(fg));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(2, 2, 12, 12));
        p.setPen(pen(accent, 1.8));
        p.drawLine(QPointF(8, 7.3), QPointF(8, 11));
        p.drawPoint(QPointF(8, 5));
        break;
      case Glyph::SaveTask:
        p.setPen(pen(fg));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(2.2, 2.2, 11.6, 11.6), 1.5, 1.5);
        p.drawRect(QRectF(5, 2.2, 6, 3.6));
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRect(QRectF(5, 9, 6, 4.8));
        break;
      case Glyph::CheckUpdates: {
        p.setPen(pen(fg));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(2.5, 11), QPointF(2.5, 13.5));
        p.drawLine(QPointF(2.5, 13.5), QPointF(13.5, 13.5));
        p.drawLine(QPointF(13.5, 13.5), QPointF(13.5, 11));
        p.setPen(pen(accent, 1.6));
        p.drawLine(QPointF(8, 2), QPointF(8, 10));
        p.drawLine(QPointF(4.8, 7), QPointF(8, 10.2));
        p.drawLine(QPointF(11.2, 7), QPointF(8, 10.2));
        break;
      }
      case Glyph::Find:
        p.setPen(pen(fg, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(2, 2, 8, 8));
        p.drawLine(QPointF(9, 9), QPointF(14, 14));
        break;
    }
  });
}

QIcon project(const ThemeColors& c, const QString& rag, const QString& status) {
  const QColor body = status == QLatin1String("active") || status.isEmpty() ? c.fg : c.muted;
  const QColor badge = ragColor(rag);
  const QColor ring = c.panelBg;
  return draw([body, badge, ring](QPainter& p) {
    folder(p, body);
    if (!badge.isValid()) return;
    p.setPen(QPen(ring, 1.5));
    p.setBrush(badge);
    p.drawEllipse(QRectF(9.2, 8.7, 6.2, 6.2));
  });
}

}  // namespace zproject::app::Icons
