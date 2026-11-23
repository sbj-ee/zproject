#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTextBrowser;
class QToolButton;

namespace zproject::app {

class AppSession;

// The task editor in the inspector dock (plan §3.3, SPEC.md §8.1): Details
// (title, type, mode, status, % complete, priority, Markdown notes with a
// preview), Schedule (duration, start and finish for manual tasks,
// constraint and its date, deadline, actual start and finish) and History
// (GET /v1/tasks/{id}/history). It shows the cached copy at once, then the
// server's copy (whose rev is the If-Match); fields are checked with
// core/Model's validation before anything is sent, and Save hands the
// changed fields to TaskActions::saveTask. A summary's status, % complete and
// actual dates are rolled up and read-only. Offline it is read-only.
class TaskInspector : public QWidget {
  Q_OBJECT
 public:
  using SessionFn = std::function<AppSession*()>;
  explicit TaskInspector(SessionFn session, QWidget* parent = nullptr);

  // Shows `id`. `cached` is the cache's copy; `children` the number of live
  // subtasks (a summary when > 0). The same task again: an untouched editor
  // follows the server, an edited one keeps the user's edits.
  void showTask(const QString& id, const QJsonObject& cached, const QString& wbs, int children);
  void setEditable(bool editable);
  void setBusy(bool busy);
  // After a save or a resolved conflict: read the server's copy again.
  void reloadFromServer();
  void revert();
  // Checks the fields; if they pass, emits saveRequested. False when there
  // is nothing to save or a field is wrong.
  bool save();

  QString taskId() const { return id_; }
  QString taskLabel() const;
  QString projectId() const { return base_.value(QStringLiteral("project")).toString(); }
  bool isSummary() const { return children_ > 0; }
  bool isDirty() const;
  bool isLoading() const { return loading_; }
  bool canSave() const;
  const QJsonObject& base() const { return base_; }
  // Every field the editor can change, as the API spells it (empty dates are
  // null; a summary's rolled-up fields are left out).
  QJsonObject values() const;
  QJsonObject body() const;  // the changed fields only
  bool validate();
  void showFieldError(const QString& field, const QString& message);
  void clearErrors();
  void setMessage(const QString& text, bool error = false);
  void loadHistory();

  QTabWidget* tabs() const { return tabs_; }
  QWidget* field(const QString& name) const { return fields_.value(name); }
  QLabel* errorLabel(const QString& name) const { return errors_.value(name); }
  QLabel* header() const { return header_; }
  QLabel* rollupNote() const { return rollupNote_; }
  QLabel* message() const { return message_; }
  QPushButton* saveButton() const { return save_; }
  QPushButton* revertButton() const { return revert_; }
  QToolButton* previewButton() const { return preview_; }
  QTextBrowser* notesPreview() const { return notesPreview_; }
  QTableWidget* historyTable() const { return history_; }
  QLabel* historyStatus() const { return historyStatus_; }
  // Tests and screenshots: type into a line edit or the notes.
  void setText(const QString& name, const QString& text);
  QString text(const QString& name) const;

 signals:
  void saveRequested(const QString& id, const QJsonObject& base, const QJsonObject& body);
  void dirtyChanged(bool dirty);

 private:
  QFormLayout* addTab(const QString& title, const QString& objectName);
  void addRow(QFormLayout* form, const QString& name, QWidget* w);
  QLineEdit* addDate(QFormLayout* form, const QString& name);
  void load(const QJsonObject& t);
  void fetch();
  void updateState();
  void edited();
  void fillHistory(const QList<QJsonObject>& rows);
  void fetchHistory(qint64 since, int page, QList<QJsonObject> rows, quint64 gen);
  QString who(const QString& actor) const;

  SessionFn session_;
  QString id_;
  QString wbs_;
  QJsonObject base_;
  int children_ = 0;
  bool editable_ = false;
  bool busy_ = false;
  bool loading_ = false;
  bool filling_ = false;
  bool wasDirty_ = false;
  bool historyLoaded_ = false;
  quint64 gen_ = 0;  // the task shown; answers for an older one are dropped

  QLabel* header_ = nullptr;
  QLabel* rollupNote_ = nullptr;
  QTabWidget* tabs_ = nullptr;
  QHash<QString, QWidget*> fields_;
  QHash<QString, QLabel*> errors_;
  QLineEdit* title_ = nullptr;
  QComboBox* type_ = nullptr;
  QComboBox* mode_ = nullptr;
  QComboBox* status_ = nullptr;
  QSpinBox* percent_ = nullptr;
  QSpinBox* priority_ = nullptr;
  QPlainTextEdit* notes_ = nullptr;
  QToolButton* preview_ = nullptr;
  QTextBrowser* notesPreview_ = nullptr;
  QDoubleSpinBox* duration_ = nullptr;
  QComboBox* constraint_ = nullptr;
  QTableWidget* history_ = nullptr;
  QLabel* historyStatus_ = nullptr;
  QLabel* message_ = nullptr;
  QPushButton* save_ = nullptr;
  QPushButton* revert_ = nullptr;
};

}  // namespace zproject::app
