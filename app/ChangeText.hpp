#pragma once

#include <QJsonObject>
#include <QString>

namespace zproject::app {

// How the app words one change record (GET /v1/tasks/{id}/history and the
// change feed), shared by the task editor's History tab and the activity
// view so both say the same thing.

// An ISO time from the server as local short date and time.
QString changeLocalTime(const QString& iso);
// One change as a line: "Created “Drawings”", "Deleted", "Restored", or
// "Finish: 2026-09-11 → 2026-09-15; Status: To do → In progress". A
// project's fields use the project labels ("Target finish").
QString describeChange(const QString& entityKind, const QJsonObject& before, const QJsonObject& after);
// Who made it: "system", "This computer" (this device's own token), or the
// token label cut to fit ("zpt_a1b2c3d4…"). ownTokenLabel is this device's
// "zpt_<id>" label, or empty.
QString changeActorName(const QString& actor, const QString& ownTokenLabel);
// The whole label, for the tooltip.
QString changeActorLabel(const QString& actor);

}  // namespace zproject::app
