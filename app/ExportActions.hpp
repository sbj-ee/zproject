#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

#include "app/ProjectPrint.hpp"
#include "core/Export.hpp"

class QPrinter;
class QWidget;

namespace zproject::app {

class AppSession;

// File → Export…, Export as PDF… and Print… for the selected project
// (SPEC.md §7.9, §8.1).
//
// The data is the server's own export (GET /v1/projects/{id}/export as
// zproject JSON: tasks, dependencies and calendars) when the server
// answers. Offline, or if that request fails, it is this computer's cached
// copy, which has no dependencies or calendars. A file export then asks
// first; a printout says so under its header. The files themselves are
// written here with core/Export, the same code the server and the CLI use,
// and atomically (QSaveFile): MS Project XML, OpenDocument spreadsheet, CSV,
// zproject JSON, or a PDF of the printout.
class ExportActions : public QObject {
  Q_OBJECT
 public:
  using SessionFn = std::function<AppSession*()>;
  ExportActions(QWidget* window, SessionFn session, QObject* parent = nullptr);
  ~ExportActions() override;

  // The file kinds offered by Export…, in menu order.
  enum class Kind { Mspdi, Ods, Csv, Pdf, Json };
  static QStringList fileFilters();
  // The kind for a chosen path: its extension, else the chosen filter.
  static std::optional<Kind> kindFor(const QString& path, const QString& selectedFilter);

  void exportProject(const QString& projectId);  // asks for a file and a format
  void exportPdf(const QString& projectId);      // asks for a .pdf file
  void print(const QString& projectId);          // the print dialog

  // Writes the project to `path` as `kind`, without asking where.
  void exportTo(const QString& projectId, const QString& path, Kind kind);

  bool busy() const { return busy_; }

  // Tests and scripted use replace the dialogs. The save dialog gets the
  // suggested path and the filters and returns the chosen path (empty:
  // cancelled) and filter. The print dialog may set up the printer and
  // returns false when cancelled. The question returns true to go on.
  using SavePathFn = std::function<QString(const QString& suggested, const QStringList& filters, QString* filter)>;
  using PrintDialogFn = std::function<bool(QPrinter* printer)>;
  using ConfirmFn = std::function<bool(const QString& title, const QString& text)>;
  void setSavePathProvider(SavePathFn f) { savePath_ = std::move(f); }
  void setPrintDialogProvider(PrintDialogFn f) { printDialog_ = std::move(f); }
  void setConfirmProvider(ConfirmFn f) { confirm_ = std::move(f); }
  void setToday(const QDate& d) { today_ = d; }  // the printout's today line

 signals:
  // Something for the status bar (or a message box when `error`).
  void notice(const QString& text, bool error);
  // An export or print ended; `path` is the file ("" for a printer).
  void finished(const QString& path, bool ok);

 private:
  struct Snapshot {
    ExportData data;
    bool fromCache = false;
    QString note;  // why the snapshot is incomplete, for a printout
  };
  // Gets the snapshot (server first, then the cache) and calls `then`;
  // `then` isn't called when there's nothing to export.
  void withSnapshot(const QString& projectId, std::function<void(const Snapshot&)> then);
  std::optional<Snapshot> cacheSnapshot(const QString& projectId, const QString& why) const;
  PrintSettings printSettings(const Snapshot& s) const;
  bool writePdf(const Snapshot& s, const QString& path, QString* error) const;
  QString suggestedPath(const QString& projectId, Kind kind) const;
  QString projectName(const QString& projectId) const;
  void done(const QString& path, bool ok, const QString& text);

  QWidget* window_;
  SessionFn session_;
  SavePathFn savePath_;
  PrintDialogFn printDialog_;
  ConfirmFn confirm_;
  QDate today_;
  QString lastDir_;
  Kind lastKind_ = Kind::Mspdi;
  bool busy_ = false;
  std::unique_ptr<QPrinter> printer_;  // alive from the dialog to the end of printing
};

}  // namespace zproject::app
