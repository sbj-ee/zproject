#include "app/EditDialogs.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core/Model.hpp"

namespace zproject::app {

namespace {

const QStringList& multiLineFields() {
  static const QStringList f{QStringLiteral("description"), QStringLiteral("objectives"), QStringLiteral("scope"),
                             QStringLiteral("outOfScope"), QStringLiteral("successCriteria")};
  return f;
}

QTableWidget* makeTable(QWidget* parent, const QString& name, const QStringList& headers) {
  auto* t = new QTableWidget(0, static_cast<int>(headers.size()), parent);
  t->setObjectName(name);
  t->setHorizontalHeaderLabels(headers);
  t->verticalHeader()->hide();
  t->setEditTriggers(QAbstractItemView::NoEditTriggers);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  t->setWordWrap(true);
  t->horizontalHeader()->setStretchLastSection(true);
  return t;
}

QTableWidgetItem* cell(const QString& text) {
  auto* item = new QTableWidgetItem(text);
  item->setToolTip(text);
  return item;
}

}  // namespace

QString fieldLabel(const QString& field) {
  static const QHash<QString, QString> labels{
      {QStringLiteral("name"), QObject::tr("Name")},
      {QStringLiteral("description"), QObject::tr("Description")},
      {QStringLiteral("objectives"), QObject::tr("Objectives")},
      {QStringLiteral("scope"), QObject::tr("Scope")},
      {QStringLiteral("outOfScope"), QObject::tr("Out of scope")},
      {QStringLiteral("successCriteria"), QObject::tr("Success criteria")},
      {QStringLiteral("owner"), QObject::tr("Owner")},
      {QStringLiteral("sponsor"), QObject::tr("Sponsor")},
      {QStringLiteral("status"), QObject::tr("Status")},
      {QStringLiteral("rag"), QObject::tr("RAG")},
      {QStringLiteral("priority"), QObject::tr("Priority")},
      {QStringLiteral("start"), QObject::tr("Start")},
      {QStringLiteral("finish"), QObject::tr("Target finish")},
      {QStringLiteral("budget"), QObject::tr("Budget")},
      {QStringLiteral("color"), QObject::tr("Colour")},
      {QStringLiteral("title"), QObject::tr("Title")},
      {QStringLiteral("percentComplete"), QObject::tr("% complete")},
      {QStringLiteral("deleted"), QObject::tr("Deleted")},
      {QStringLiteral("type"), QObject::tr("Type")},
      {QStringLiteral("duration"), QObject::tr("Duration")},
      {QStringLiteral("parent"), QObject::tr("Parent")},
      {QStringLiteral("order"), QObject::tr("Position")},
      {QStringLiteral("deadline"), QObject::tr("Deadline")},
      {QStringLiteral("actualStart"), QObject::tr("Actual start")},
      {QStringLiteral("actualFinish"), QObject::tr("Actual finish")},
  };
  return labels.value(field, field);
}

QString ragLetter(const QString& rag) {
  if (rag == QLatin1String("green")) return QStringLiteral("G");
  if (rag == QLatin1String("amber")) return QStringLiteral("A");
  if (rag == QLatin1String("red")) return QStringLiteral("R");
  return QStringLiteral("–");
}

QString projectStatusName(const QString& status) {
  if (status == QLatin1String("active")) return QObject::tr("Active");
  if (status == QLatin1String("on_hold")) return QObject::tr("On hold");
  if (status == QLatin1String("done")) return QObject::tr("Done");
  if (status == QLatin1String("cancelled")) return QObject::tr("Cancelled");
  return status;
}

// ---- PreviewDialog ----

PreviewDialog::PreviewDialog(const QString& title, const QString& intro, const QList<Row>& rows,
                             const QString& confirmText, QWidget* parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("previewDialog"));
  setWindowTitle(title);
  setAccessibleName(title);
  auto* v = new QVBoxLayout(this);
  intro_ = new QLabel(intro, this);
  intro_->setObjectName(QStringLiteral("previewIntro"));
  intro_->setWordWrap(true);
  v->addWidget(intro_);
  table_ = makeTable(this, QStringLiteral("previewTable"), {tr("Change"), tr("What"), tr("Details")});
  table_->setAccessibleName(tr("Changes the server will make"));
  for (const Row& r : rows) {
    const int i = table_->rowCount();
    table_->insertRow(i);
    QString op = r.op;
    if (!op.isEmpty()) op[0] = op[0].toUpper();
    table_->setItem(i, 0, cell(op));
    table_->setItem(i, 1, cell(QStringLiteral("%1 %2").arg(r.kind == QLatin1String("project") ? tr("Project") : tr("Task"), r.name)));
    table_->setItem(i, 2, cell(r.detail));
  }
  table_->resizeColumnsToContents();
  v->addWidget(table_, 1);
  auto* buttons = new QDialogButtonBox(this);
  confirm_ = buttons->addButton(confirmText, QDialogButtonBox::AcceptRole);
  confirm_->setObjectName(QStringLiteral("previewConfirm"));
  cancel_ = buttons->addButton(QDialogButtonBox::Cancel);
  cancel_->setObjectName(QStringLiteral("previewCancel"));
  cancel_->setDefault(true);  // a destructive action is never the default
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  v->addWidget(buttons);
  resize(620, 360);
}

// ---- ConflictDialog ----

ConflictDialog::ConflictDialog(const QString& what, const client::ConflictMerge& merge, const QString& who,
                               QWidget* parent)
    : QDialog(parent) {
  using Kind = client::ConflictMerge::Kind;
  setObjectName(QStringLiteral("conflictDialog"));
  setWindowTitle(tr("Changed on the Server"));
  setAccessibleName(windowTitle());
  auto* v = new QVBoxLayout(this);
  const int n = merge.conflicts();
  QString text = tr("Someone else changed %1 while you were editing it.").arg(what);
  if (!who.isEmpty()) text += QLatin1Char(' ') + who + QLatin1Char('.');
  text += QStringLiteral("\n\n");
  text += n == 0 ? tr("None of your changes clash with theirs. Keep Mine applies your changes on top of the "
                      "server's version; Take Theirs drops them.")
                 : (n == 1 ? tr("1 field was changed on both sides: choose whose value to keep. ")
                           : tr("%1 fields were changed on both sides: choose whose values to keep. ").arg(n)) +
                       tr("Fields only you changed are kept either way.");
  summary_ = new QLabel(text, this);
  summary_->setObjectName(QStringLiteral("conflictSummary"));
  summary_->setWordWrap(true);
  v->addWidget(summary_);

  table_ = makeTable(this, QStringLiteral("conflictTable"), {tr("Field"), tr("Your value"), tr("Server value"), tr("Result")});
  table_->setAccessibleName(tr("Fields that differ from the version you started from"));
  for (const auto& f : merge.fields()) {
    const int i = table_->rowCount();
    table_->insertRow(i);
    QString result;
    switch (f.kind) {
      case Kind::Mine:
        result = tr("Yours kept (only you changed it)");
        break;
      case Kind::Theirs:
        result = tr("Server's kept (only they changed it)");
        break;
      case Kind::Same:
        result = tr("Same on both sides");
        break;
      case Kind::Conflict:
        result = tr("⚠ Both changed: choose");
        break;
    }
    table_->setItem(i, 0, cell(fieldLabel(f.name)));
    table_->setItem(i, 1, cell(client::displayValue(f.mine)));
    table_->setItem(i, 2, cell(client::displayValue(f.theirs)));
    auto* r = cell(result);
    if (f.kind == Kind::Conflict) {
      QFont bold = r->font();
      bold.setBold(true);
      for (int c = 0; c < 3; ++c) table_->item(i, c)->setFont(bold);
      r->setFont(bold);
    }
    r->setData(Qt::UserRole, client::ConflictMerge::kindName(f.kind));
    table_->setItem(i, 3, r);
  }
  // Field and Result fit their text; the two values share the rest and wrap.
  table_->setWordWrap(true);
  table_->setTextElideMode(Qt::ElideNone);
  QHeaderView* h = table_->horizontalHeader();
  h->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  h->setSectionResizeMode(1, QHeaderView::Stretch);
  h->setSectionResizeMode(2, QHeaderView::Stretch);
  h->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  table_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  v->addWidget(table_, 1);
  resize(std::max(width(), 760), height());

  auto* buttons = new QDialogButtonBox(this);
  keepMine_ = buttons->addButton(tr("Keep Mine"), QDialogButtonBox::AcceptRole);
  keepMine_->setObjectName(QStringLiteral("conflictKeepMine"));
  keepMine_->setToolTip(tr("Save your values on top of the server's newer version"));
  takeTheirs_ = buttons->addButton(tr("Take Theirs"), QDialogButtonBox::ActionRole);
  takeTheirs_->setObjectName(QStringLiteral("conflictTakeTheirs"));
  takeTheirs_->setToolTip(tr("Keep the server's values where both changed; your other changes are still saved"));
  cancel_ = buttons->addButton(QDialogButtonBox::Cancel);
  cancel_->setObjectName(QStringLiteral("conflictCancel"));
  cancel_->setToolTip(tr("Send nothing and go back to editing"));
  cancel_->setDefault(true);
  connect(keepMine_, &QPushButton::clicked, this, [this] { choose(Choice::KeepMine); });
  connect(takeTheirs_, &QPushButton::clicked, this, [this] { choose(Choice::TakeTheirs); });
  connect(cancel_, &QPushButton::clicked, this, [this] { choose(Choice::Cancel); });
  connect(this, &QDialog::rejected, this, [this] {
    if (choice_ == Choice::Cancel) emit chosen(Choice::Cancel);
  });
  v->addWidget(buttons);
  resize(720, 380);
}

void ConflictDialog::choose(Choice c) {
  choice_ = c;
  if (c == Choice::Cancel) {
    reject();  // emits chosen(Cancel) once, from rejected
    return;
  }
  emit chosen(c);
  accept();
}

// ---- ProjectEditor ----

ProjectEditor::ProjectEditor(const QJsonObject& base, QWidget* parent) : QDialog(parent), base_(base) {
  setObjectName(QStringLiteral("projectEditor"));
  setWindowTitle(isNew() ? tr("New Project")
                         : tr("Edit Project — %1").arg(base.value(QStringLiteral("name")).toString()));
  setAccessibleName(windowTitle());
  auto* outer = new QVBoxLayout(this);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* inner = new QWidget(scroll);
  auto* columns = new QHBoxLayout(inner);
  auto* left = new QFormLayout;
  auto* right = new QFormLayout;
  columns->addLayout(left, 1);
  columns->addSpacing(16);
  columns->addLayout(right, 1);

  addLine(left, QStringLiteral("name"));
  status_ = new QComboBox(this);
  for (const char* s : {"active", "on_hold", "done", "cancelled"}) {
    status_->addItem(projectStatusName(QString::fromLatin1(s)), QString::fromLatin1(s));
  }
  addRow(left, QStringLiteral("status"), status_);
  rag_ = new QComboBox(this);
  rag_->addItem(tr("G — Green (on track)"), QStringLiteral("green"));
  rag_->addItem(tr("A — Amber (at risk)"), QStringLiteral("amber"));
  rag_->addItem(tr("R — Red (in trouble)"), QStringLiteral("red"));
  addRow(left, QStringLiteral("rag"), rag_);
  priority_ = new QSpinBox(this);
  priority_->setRange(1, 4);
  priority_->setToolTip(tr("1 = highest, 4 = lowest"));
  addRow(left, QStringLiteral("priority"), priority_);
  addLine(left, QStringLiteral("start"));
  addLine(left, QStringLiteral("finish"));
  addLine(left, QStringLiteral("owner"));
  addLine(left, QStringLiteral("sponsor"));
  addLine(left, QStringLiteral("budget"));
  addLine(left, QStringLiteral("color"));
  for (const char* d : {"start", "finish"}) {
    qobject_cast<QLineEdit*>(fields_.value(QString::fromLatin1(d)))->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
  }
  qobject_cast<QLineEdit*>(fields_.value(QStringLiteral("color")))->setPlaceholderText(QStringLiteral("#rrggbb"));
  qobject_cast<QLineEdit*>(fields_.value(QStringLiteral("budget")))->setPlaceholderText(tr("none"));
  for (const QString& f : multiLineFields()) addText(right, f);

  scroll->setWidget(inner);
  outer->addWidget(scroll, 1);
  message_ = new QLabel(this);
  message_->setObjectName(QStringLiteral("editorMessage"));
  message_->setWordWrap(true);
  message_->hide();
  outer->addWidget(message_);
  auto* buttons = new QDialogButtonBox(this);
  save_ = buttons->addButton(isNew() ? tr("Create Project") : tr("Save"), QDialogButtonBox::AcceptRole);
  save_->setObjectName(QStringLiteral("editorSave"));
  save_->setDefault(true);
  QPushButton* cancel = buttons->addButton(QDialogButtonBox::Cancel);
  cancel->setObjectName(QStringLiteral("editorCancel"));
  connect(save_, &QPushButton::clicked, this, [this] {
    if (validate()) emit saveRequested();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  outer->addWidget(buttons);

  if (!isNew()) {
    load(base_);
  } else {
    status_->setCurrentIndex(0);
    rag_->setCurrentIndex(0);
    priority_->setValue(2);
  }
  resize(860, 560);
}

void ProjectEditor::addRow(QFormLayout* form, const QString& name, QWidget* w) {
  w->setObjectName(QStringLiteral("field-") + name);
  w->setAccessibleName(fieldLabel(name));
  auto* box = new QWidget(this);
  auto* v = new QVBoxLayout(box);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(2);
  v->addWidget(w);
  auto* err = new QLabel(box);
  err->setObjectName(QStringLiteral("error-") + name);
  err->setWordWrap(true);
  err->hide();
  v->addWidget(err);
  auto* label = new QLabel(fieldLabel(name) + QLatin1Char(':'), this);
  label->setBuddy(w);
  form->addRow(label, box);
  fields_.insert(name, w);
  errors_.insert(name, err);
}

void ProjectEditor::addLine(QFormLayout* form, const QString& name) { addRow(form, name, new QLineEdit(this)); }

void ProjectEditor::addText(QFormLayout* form, const QString& name) {
  auto* e = new QPlainTextEdit(this);
  e->setTabChangesFocus(true);
  e->setMinimumHeight(64);
  e->setPlaceholderText(tr("Markdown"));
  addRow(form, name, e);
}

void ProjectEditor::setText(const QString& name, const QString& value) {
  if (auto* l = qobject_cast<QLineEdit*>(fields_.value(name))) l->setText(value);
  if (auto* t = qobject_cast<QPlainTextEdit*>(fields_.value(name))) t->setPlainText(value);
}

QString ProjectEditor::text(const QString& name) const {
  if (auto* l = qobject_cast<QLineEdit*>(fields_.value(name))) return l->text().trimmed();
  if (auto* t = qobject_cast<QPlainTextEdit*>(fields_.value(name))) return t->toPlainText();
  return {};
}

void ProjectEditor::load(const QJsonObject& p) {
  for (const char* f : {"name", "start", "finish", "owner", "sponsor", "color", "description", "objectives", "scope",
                        "outOfScope", "successCriteria"}) {
    setText(QString::fromLatin1(f), p.value(QLatin1String(f)).toString());
  }
  const QJsonValue budget = p.value(QStringLiteral("budget"));
  setText(QStringLiteral("budget"), budget.isDouble() ? client::displayValue(budget) : QString());
  status_->setCurrentIndex(std::max(0, status_->findData(p.value(QStringLiteral("status")).toString())));
  rag_->setCurrentIndex(std::max(0, rag_->findData(p.value(QStringLiteral("rag")).toString())));
  priority_->setValue(p.value(QStringLiteral("priority")).toInt(2));
}

QJsonObject ProjectEditor::values() const {
  QJsonObject o;
  for (const char* f : {"name", "owner", "sponsor", "description", "objectives", "scope", "outOfScope",
                        "successCriteria"}) {
    o.insert(QLatin1String(f), text(QString::fromLatin1(f)));
  }
  auto optional = [this, &o](const char* f) {
    const QString v = text(QString::fromLatin1(f));
    o.insert(QLatin1String(f), v.isEmpty() ? QJsonValue() : QJsonValue(v));
  };
  optional("start");
  optional("finish");
  optional("color");
  const QString budget = text(QStringLiteral("budget"));
  bool ok = false;
  const double b = budget.toDouble(&ok);
  o.insert(QStringLiteral("budget"), budget.isEmpty() ? QJsonValue() : (ok ? QJsonValue(b) : QJsonValue(budget)));
  o.insert(QStringLiteral("status"), status_->currentData().toString());
  o.insert(QStringLiteral("rag"), rag_->currentData().toString());
  o.insert(QStringLiteral("priority"), priority_->value());
  return o;
}

QJsonObject ProjectEditor::body() const {
  const QJsonObject v = values();
  if (!isNew()) return client::changedBody(base_, v);
  QJsonObject out;
  for (auto it = v.constBegin(); it != v.constEnd(); ++it) {
    if (it.value().isNull() || (it.value().isString() && it.value().toString().isEmpty())) continue;
    out.insert(it.key(), it.value());
  }
  return out;
}

bool ProjectEditor::validate() {
  clearErrors();
  const QJsonObject v = values();
  bool ok = true;
  if (v.value(QStringLiteral("budget")).isString()) {
    showFieldError(QStringLiteral("budget"), tr("must be a number"));
    ok = false;
  }
  QJsonObject merged = base_;
  for (auto it = v.constBegin(); it != v.constEnd(); ++it) merged.insert(it.key(), it.value());
  if (!ok) merged.remove(QStringLiteral("budget"));
  for (const FieldError& e : validateProject(projectFromJson(merged))) {
    showFieldError(e.field, e.message);
    ok = false;
  }
  if (!ok) setMessage(tr("Fix the fields marked ⚠ and save again. Nothing was sent."));
  return ok;
}

void ProjectEditor::showFieldError(const QString& field, const QString& message) {
  QLabel* l = errors_.value(field);
  if (l == nullptr) {
    setMessage(QStringLiteral("⚠ %1: %2").arg(fieldLabel(field), message));
    return;
  }
  l->setText(QStringLiteral("⚠ %1 %2").arg(fieldLabel(field), message));
  l->setAccessibleName(l->text());
  l->show();
  if (QWidget* w = fields_.value(field)) w->setAccessibleDescription(l->text());
}

void ProjectEditor::clearErrors() {
  for (auto it = errors_.constBegin(); it != errors_.constEnd(); ++it) {
    it.value()->clear();
    it.value()->hide();
    if (QWidget* w = fields_.value(it.key())) w->setAccessibleDescription(QString());
  }
  setMessage(QString());
}

void ProjectEditor::setMessage(const QString& text) {
  message_->setText(text);
  message_->setAccessibleName(text);
  message_->setVisible(!text.isEmpty());
}

void ProjectEditor::setLoading(bool loading) {
  loading_ = loading;
  for (QWidget* f : std::as_const(fields_)) f->setEnabled(!loading);
  save_->setEnabled(!loading);
  if (loading) setMessage(tr("Getting the server's copy…"));
}

void ProjectEditor::reload(const QJsonObject& server) {
  base_ = server;
  load(server);
  setLoading(false);
  setMessage(QString());
}

void ProjectEditor::setBusy(bool busy) {
  if (loading_) return;
  save_->setEnabled(!busy);
  save_->setText(busy ? tr("Saving…") : (isNew() ? tr("Create Project") : tr("Save")));
}

}  // namespace zproject::app
