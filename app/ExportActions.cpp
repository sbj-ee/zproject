#include "app/ExportActions.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>

#include "app/AppSession.hpp"
#include "client/ApiError.hpp"
#include "client/AsyncApiClient.hpp"
#include "client/Cache.hpp"
#include "core/Version.hpp"

namespace zproject::app {
namespace {

QString extensionOf(ExportActions::Kind k) {
  switch (k) {
    case ExportActions::Kind::Mspdi: return QStringLiteral("xml");
    case ExportActions::Kind::Ods: return QStringLiteral("ods");
    case ExportActions::Kind::Csv: return QStringLiteral("csv");
    case ExportActions::Kind::Pdf: return QStringLiteral("pdf");
    case ExportActions::Kind::Json: return QStringLiteral("json");
  }
  return {};
}

ExportFormat formatOf(ExportActions::Kind k) {
  switch (k) {
    case ExportActions::Kind::Ods: return ExportFormat::Ods;
    case ExportActions::Kind::Csv: return ExportFormat::Csv;
    case ExportActions::Kind::Json: return ExportFormat::Json;
    case ExportActions::Kind::Mspdi:
    case ExportActions::Kind::Pdf: break;
  }
  return ExportFormat::Mspdi;
}

const QList<ExportActions::Kind>& kinds() {
  static const QList<ExportActions::Kind> k{ExportActions::Kind::Mspdi, ExportActions::Kind::Ods,
                                            ExportActions::Kind::Csv, ExportActions::Kind::Pdf,
                                            ExportActions::Kind::Json};
  return k;
}

// Letter where the locale measures in inches (the US), else A4.
QPageSize localPageSize() {
  return QLocale::system().measurementSystem() == QLocale::ImperialUSSystem ? QPageSize(QPageSize::Letter)
                                                                            : QPageSize(QPageSize::A4);
}

}  // namespace

ExportActions::ExportActions(QWidget* window, SessionFn session, QObject* parent)
    : QObject(parent), window_(window), session_(std::move(session)) {}

ExportActions::~ExportActions() = default;

QStringList ExportActions::fileFilters() {
  return {tr("MS Project XML (*.xml)"), tr("OpenDocument spreadsheet (*.ods)"), tr("CSV (*.csv)"),
          tr("PDF (*.pdf)"), tr("zproject JSON (*.json)")};
}

std::optional<ExportActions::Kind> ExportActions::kindFor(const QString& path, const QString& selectedFilter) {
  const QString ext = QFileInfo(path).suffix().toLower();
  for (Kind k : kinds()) {
    if (ext == extensionOf(k)) return k;
  }
  const qsizetype i = fileFilters().indexOf(selectedFilter);
  if (i >= 0) return kinds().at(i);
  return std::nullopt;
}

QString ExportActions::projectName(const QString& projectId) const {
  AppSession* s = session_();
  if (s != nullptr && s->cache() != nullptr) {
    if (auto p = s->cache()->project(projectId)) return p->value(QStringLiteral("name")).toString();
  }
  return {};
}

QString ExportActions::suggestedPath(const QString& projectId, Kind kind) const {
  QString dir = lastDir_;
  if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (dir.isEmpty()) dir = QDir::homePath();
  QString name = exportFileName(projectName(projectId), ExportFormat::Json);
  name.chop(4);  // "json"
  return QDir(dir).filePath(name + extensionOf(kind));
}

void ExportActions::exportProject(const QString& projectId) {
  if (projectId.isEmpty() || busy_) return;
  const QStringList filters = fileFilters();
  QString filter = filters.at(kinds().indexOf(lastKind_));
  const QString suggested = suggestedPath(projectId, lastKind_);
  QString path = savePath_ ? savePath_(suggested, filters, &filter)
                           : QFileDialog::getSaveFileName(window_, tr("Export Project"), suggested,
                                                          filters.join(QStringLiteral(";;")), &filter);
  if (path.isEmpty()) return;
  const auto kind = kindFor(path, filter);
  if (!kind) {
    emit notice(tr("Choose a file type to export."), true);
    return;
  }
  if (QFileInfo(path).suffix().toLower() != extensionOf(*kind)) path += QLatin1Char('.') + extensionOf(*kind);
  lastKind_ = *kind;
  exportTo(projectId, path, *kind);
}

void ExportActions::exportPdf(const QString& projectId) {
  if (projectId.isEmpty() || busy_) return;
  const QString filter = fileFilters().at(kinds().indexOf(Kind::Pdf));
  const QString suggested = suggestedPath(projectId, Kind::Pdf);
  QString chosen = filter;
  QString path = savePath_ ? savePath_(suggested, {filter}, &chosen)
                           : QFileDialog::getSaveFileName(window_, tr("Export as PDF"), suggested, filter, &chosen);
  if (path.isEmpty()) return;
  if (QFileInfo(path).suffix().toLower() != QLatin1String("pdf")) path += QStringLiteral(".pdf");
  exportTo(projectId, path, Kind::Pdf);
}

void ExportActions::exportTo(const QString& projectId, const QString& path, Kind kind) {
  if (busy_) return;
  lastDir_ = QFileInfo(path).absolutePath();
  busy_ = true;
  withSnapshot(projectId, [this, path, kind](const Snapshot& s) {
    if (s.fromCache && kind != Kind::Pdf) {
      const QString text =
          tr("%1\n\nThe file will have the project and its tasks from this computer's copy, but no dependencies "
             "or calendars. Export anyway?")
              .arg(s.note);
      const bool go = confirm_ ? confirm_(tr("Export from the offline copy?"), text)
                               : QMessageBox::question(window_, tr("Export from the offline copy?"), text) ==
                                     QMessageBox::Yes;
      if (!go) {
        done(path, false, QString());
        return;
      }
    }
    QString error;
    bool ok = false;
    if (kind == Kind::Pdf) {
      ok = writePdf(s, path, &error);
    } else {
      QSaveFile f(path);
      const QByteArray bytes = zproject::exportProject(s.data, formatOf(kind));
      ok = f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
      if (!ok) error = f.errorString();
    }
    const QString file = QDir::toNativeSeparators(path);
    done(path, ok,
         ok ? tr("Exported %1 to %2.").arg(s.data.project.name, file)
            : tr("Couldn't write %1: %2").arg(file, error));
  });
}

void ExportActions::print(const QString& projectId) {
  if (projectId.isEmpty() || busy_) return;
  printer_ = std::make_unique<QPrinter>(QPrinter::HighResolution);
  printer_->setPageSize(localPageSize());
  printer_->setPageOrientation(QPageLayout::Landscape);
  printer_->setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);
  printer_->setDocName(projectName(projectId));
  printer_->setCreator(versionLine(QStringLiteral("zproject")));
  bool go = false;
  if (printDialog_) {
    go = printDialog_(printer_.get());
  } else {
    QPrintDialog dialog(printer_.get(), window_);
    dialog.setWindowTitle(tr("Print Project"));
    go = dialog.exec() == QDialog::Accepted;
  }
  if (!go) {
    printer_.reset();
    return;
  }
  busy_ = true;
  withSnapshot(projectId, [this](const Snapshot& s) {
    ProjectPrintout out(s.data, printSettings(s));
    const bool ok = out.print(printer_.get());
    const QString file = printer_->outputFileName();
    printer_.reset();
    done(file, ok,
         ok ? tr("Sent %1 to the printer (%n page(s)).", nullptr, out.pageCount()).arg(s.data.project.name)
            : tr("Couldn't print %1.").arg(s.data.project.name));
  });
}

void ExportActions::withSnapshot(const QString& projectId, std::function<void(const Snapshot&)> then) {
  AppSession* s = session_();
  auto fromCache = [this, projectId, then](const QString& why) {
    auto snap = cacheSnapshot(projectId, why);
    if (!snap) {
      done(QString(), false, tr("This project isn't on this computer yet; connect to the server and try again."));
      return;
    }
    then(*snap);
  };
  if (s == nullptr || s->api() == nullptr || s->health() != AppSession::Health::Online) {
    fromCache(tr("Offline"));
    return;
  }
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
  QPointer<ExportActions> self(this);
  s->api()->get(QStringLiteral("/v1/projects/%1/export").arg(projectId), q,
                [self, then, fromCache](const client::ApiResponse& r) {
                  if (!self) return;
                  if (r.ok()) {
                    auto data = exportDataFromJson(r.body);
                    if (data) {
                      then(Snapshot{data.value(), false, QString()});
                      return;
                    }
                  }
                  const client::ApiError e = client::classifyResponse(r);
                  fromCache(r.ok() ? tr("The server's export couldn't be read") : e.message);
                });
}

std::optional<ExportActions::Snapshot> ExportActions::cacheSnapshot(const QString& projectId,
                                                                    const QString& why) const {
  AppSession* s = session_();
  if (s == nullptr || s->cache() == nullptr) return std::nullopt;
  const auto p = s->cache()->project(projectId);
  if (!p) return std::nullopt;
  Snapshot snap;
  snap.fromCache = true;
  snap.data.project = projectFromJson(*p);
  for (const QJsonObject& t : s->cache()->tasks(projectId)) snap.data.tasks.append(taskFromJson(t));
  snap.data.exportedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  snap.data.generator = versionLine(QStringLiteral("zproject"));
  const QDateTime when = s->lastRefresh();
  snap.note = when.isValid()
                  ? tr("%1: from this computer's copy of %2, without dependencies.")
                        .arg(why, QLocale().toString(when, QLocale::ShortFormat))
                  : tr("%1: from this computer's copy, without dependencies.").arg(why);
  return snap;
}

PrintSettings ExportActions::printSettings(const Snapshot& s) const {
  PrintSettings p;
  p.subtitle = tr("Printed %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
  p.note = s.note;
  p.footer = versionLine(QStringLiteral("zproject"));
  p.today = today_.isValid() ? today_ : QDate::currentDate();
  return p;
}

bool ExportActions::writePdf(const Snapshot& s, const QString& path, QString* error) const {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    *error = f.errorString();
    return false;
  }
  {
    QPdfWriter pdf(&f);
    pdf.setResolution(300);
    pdf.setPageSize(localPageSize());
    pdf.setPageOrientation(QPageLayout::Landscape);
    pdf.setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);
    pdf.setTitle(s.data.project.name);
    pdf.setCreator(versionLine(QStringLiteral("zproject")));
    ProjectPrintout out(s.data, printSettings(s));
    if (!out.print(&pdf)) {
      *error = tr("the PDF couldn't be drawn");
      f.cancelWriting();
      return false;
    }
  }
  if (!f.commit()) {
    *error = f.errorString();
    return false;
  }
  return true;
}

void ExportActions::done(const QString& path, bool ok, const QString& text) {
  busy_ = false;
  if (!text.isEmpty()) emit notice(text, !ok);
  emit finished(path, ok);
}

}  // namespace zproject::app
