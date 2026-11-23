#include "app/ChangeText.hpp"

#include <QDateTime>
#include <QLocale>
#include <QObject>
#include <QStringList>

#include "app/EditDialogs.hpp"
#include "client/EditFlow.hpp"

namespace zproject::app {

QString changeLocalTime(const QString& iso) {
  const QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
  return t.isValid() ? QLocale().toString(t.toLocalTime(), QLocale::ShortFormat) : iso;
}

QString describeChange(const QString& entityKind, const QJsonObject& before, const QJsonObject& after) {
  const bool project = entityKind == QLatin1String("project");
  auto label = [project](const QString& k) { return project ? fieldLabel(k) : taskFieldLabel(k); };
  if (after.value(QStringLiteral("deleted")).toBool() && !before.value(QStringLiteral("deleted")).toBool() &&
      before.contains(QStringLiteral("deleted"))) {
    return QObject::tr("Deleted");
  }
  if (before.value(QStringLiteral("deleted")).toBool() && after.contains(QStringLiteral("deleted")) &&
      !after.value(QStringLiteral("deleted")).toBool()) {
    return QObject::tr("Restored");
  }
  const QString nameKey = project ? QStringLiteral("name") : QStringLiteral("title");
  if (before.isEmpty() && after.contains(nameKey)) {
    return QObject::tr("Created “%1”").arg(after.value(nameKey).toString());
  }
  QStringList parts;
  QStringList keys = after.keys();
  for (const QString& k : before.keys()) {
    if (!keys.contains(k)) keys.append(k);
  }
  for (const QString& k : keys) {
    if (k == QLatin1String("rev") || k == QLatin1String("updatedAt")) continue;
    if (k == QLatin1String("notes")) {
      parts.append(QObject::tr("Notes edited"));
      continue;
    }
    if (k == QLatin1String("parent") || k == QLatin1String("order")) {
      parts.append(QObject::tr("%1 changed").arg(label(k)));
      continue;
    }
    const QString b = client::displayValue(before.value(k));
    const QString a = client::displayValue(after.value(k));
    parts.append(QStringLiteral("%1: %2 → %3")
                     .arg(label(k), b.isEmpty() ? QStringLiteral("—") : b, a.isEmpty() ? QStringLiteral("—") : a));
  }
  return parts.join(QStringLiteral("; "));
}

QString changeActorLabel(const QString& actor) {
  if (actor.isEmpty() || actor == QLatin1String("system")) return QObject::tr("system");
  return QStringLiteral("zpt_") + actor;
}

QString changeActorName(const QString& actor, const QString& ownTokenLabel) {
  if (actor.isEmpty() || actor == QLatin1String("system")) return QObject::tr("system");
  const QString label = changeActorLabel(actor);
  if (!ownTokenLabel.isEmpty() && ownTokenLabel == label) return QObject::tr("This computer");
  // Long token ids are cut short; the tooltip has the whole label.
  return label.size() > 16 ? label.left(12) + QChar(0x2026) : label;
}

}  // namespace zproject::app
