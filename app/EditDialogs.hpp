#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "client/EditFlow.hpp"

class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace zproject::app {

// The user-facing name of a project or task field ("outOfScope" → "Out of
// scope"); unknown names come back as they are.
QString fieldLabel(const QString& field);
// The same for a task field: "finish" is "Finish" (a project's is "Target
// finish"), plus mode, constraint, notes and the other task-only fields.
QString taskFieldLabel(const QString& field);

// The dry-run preview sheet (plan §4.5): every change the server will make,
// shown before a destructive write or one that ripples to other entities.
// Shared by every editor.
class PreviewDialog : public QDialog {
  Q_OBJECT
 public:
  struct Row {
    QString op;      // create, update, delete, ...
    QString kind;    // project, task
    QString name;    // what the user calls it
    QString detail;  // changed fields, "deleted", ...
  };
  PreviewDialog(const QString& title, const QString& intro, const QList<Row>& rows, const QString& confirmText,
                QWidget* parent = nullptr);
  QTableWidget* table() const { return table_; }
  QPushButton* confirmButton() const { return confirm_; }
  QPushButton* cancelButton() const { return cancel_; }
  QLabel* intro() const { return intro_; }

 private:
  QTableWidget* table_ = nullptr;
  QPushButton* confirm_ = nullptr;
  QPushButton* cancel_ = nullptr;
  QLabel* intro_ = nullptr;
};

// The shared 412 conflict dialog (plan §4.4): field · your value · server
// value. Fields only you changed are kept; fields the server changed stay
// the server's; fields both changed differently need Keep Mine or Take
// Theirs. Nothing is sent until the user picks one, and never blindly.
class ConflictDialog : public QDialog {
  Q_OBJECT
 public:
  enum class Choice { KeepMine, TakeTheirs, Cancel };
  // `what` names the entity ("project Kitchen remodel"); `who` says who
  // changed it and when ("Changed by owner at 3:05 PM"), or is empty.
  ConflictDialog(const QString& what, const client::ConflictMerge& merge, const QString& who,
                 QWidget* parent = nullptr);
  QTableWidget* table() const { return table_; }
  QLabel* summary() const { return summary_; }
  QPushButton* keepMineButton() const { return keepMine_; }
  QPushButton* takeTheirsButton() const { return takeTheirs_; }
  QPushButton* cancelButton() const { return cancel_; }
  Choice choice() const { return choice_; }

 signals:
  void chosen(zproject::app::ConflictDialog::Choice choice);

 private:
  void choose(Choice c);
  QTableWidget* table_ = nullptr;
  QLabel* summary_ = nullptr;
  QPushButton* keepMine_ = nullptr;
  QPushButton* takeTheirs_ = nullptr;
  QPushButton* cancel_ = nullptr;
  Choice choice_ = Choice::Cancel;
};

// New Project / Edit Project Details (SPEC.md §7.10). It checks the fields
// with core/Model's validation before anything is sent; Save emits
// saveRequested() and the caller closes it once the server has the change.
class ProjectEditor : public QDialog {
  Q_OBJECT
 public:
  // An empty base: a new project.
  explicit ProjectEditor(const QJsonObject& base, QWidget* parent = nullptr);

  bool isNew() const { return base_.isEmpty(); }
  const QJsonObject& base() const { return base_; }
  QString projectId() const { return base_.value(QStringLiteral("id")).toString(); }
  // Every editable field as the API spells it (empty dates and colour are
  // null, an empty budget is null).
  QJsonObject values() const;
  // What to send: the changed fields (edit) or the filled-in ones (new).
  QJsonObject body() const;
  // Client-side checks; shows each problem next to its field.
  bool validate();
  void showFieldError(const QString& field, const QString& message);
  void clearErrors();
  void setMessage(const QString& text);
  void setBusy(bool busy);
  // While the server's copy is being fetched: nothing can be typed or saved.
  void setLoading(bool loading);
  bool isLoading() const { return loading_; }
  // The server's copy (with its rev) becomes the base and fills the fields.
  void reload(const QJsonObject& server);

  QWidget* field(const QString& name) const { return fields_.value(name); }
  QLabel* errorLabel(const QString& name) const { return errors_.value(name); }
  QLabel* message() const { return message_; }
  QPushButton* saveButton() const { return save_; }
  void setText(const QString& name, const QString& text);  // tests and screenshots

 signals:
  void saveRequested();

 private:
  void addLine(QFormLayout* form, const QString& name);
  void addText(QFormLayout* form, const QString& name);
  void addRow(QFormLayout* form, const QString& name, QWidget* w);
  void load(const QJsonObject& p);
  QString text(const QString& name) const;

  QJsonObject base_;
  QHash<QString, QWidget*> fields_;
  QHash<QString, QLabel*> errors_;
  QComboBox* status_ = nullptr;
  QComboBox* rag_ = nullptr;
  QSpinBox* priority_ = nullptr;
  QLabel* message_ = nullptr;
  QPushButton* save_ = nullptr;
  bool loading_ = false;
};

// "G", "A", "R": RAG is never colour alone (SPEC.md §18).
QString ragLetter(const QString& rag);
QString projectStatusName(const QString& status);

}  // namespace zproject::app
