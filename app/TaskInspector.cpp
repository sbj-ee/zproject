#include "app/TaskInspector.hpp"

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "app/AppSession.hpp"
#include "app/ChangeText.hpp"
#include "app/EditDialogs.hpp"
#include "client/ApiError.hpp"
#include "client/AsyncApiClient.hpp"
#include "client/EditFlow.hpp"

namespace zproject::app {

namespace {

const QString kDate = QStringLiteral("YYYY-MM-DD");
const QStringList kRolledUp = {QStringLiteral("status"), QStringLiteral("percentComplete"),
                               QStringLiteral("actualStart"), QStringLiteral("actualFinish")};
const QStringList kDates = {QStringLiteral("start"),    QStringLiteral("finish"),      QStringLiteral("constraintDate"),
                            QStringLiteral("deadline"), QStringLiteral("actualStart"), QStringLiteral("actualFinish")};
constexpr int kHistoryPage = 500;
constexpr int kHistoryPages = 4;  // 2,000 changes at most

QJsonValue orNull(const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); }

void setReadOnly(QWidget* w, bool ro) {
  if (auto* l = qobject_cast<QLineEdit*>(w)) {
    l->setReadOnly(ro);
  } else if (auto* t = qobject_cast<QPlainTextEdit*>(w)) {
    t->setReadOnly(ro);
  } else if (auto* s = qobject_cast<QAbstractSpinBox*>(w)) {
    s->setReadOnly(ro);
    s->setButtonSymbols(ro ? QAbstractSpinBox::NoButtons : QAbstractSpinBox::UpDownArrows);
  } else if (w != nullptr) {
    w->setEnabled(!ro);
  }
}

}  // namespace

TaskInspector::TaskInspector(SessionFn session, QWidget* parent) : QWidget(parent), session_(std::move(session)) {
  setObjectName(QStringLiteral("taskInspector"));
  setAccessibleName(tr("Task editor"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(8, 8, 8, 8);
  v->setSpacing(6);
  header_ = new QLabel(this);
  header_->setObjectName(QStringLiteral("inspectorHeader"));
  QFont f = header_->font();
  f.setBold(true);
  header_->setFont(f);
  header_->setWordWrap(true);
  header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(header_);
  rollupNote_ = new QLabel(this);
  rollupNote_->setObjectName(QStringLiteral("inspectorRollupNote"));
  rollupNote_->setWordWrap(true);
  rollupNote_->hide();
  v->addWidget(rollupNote_);

  tabs_ = new QTabWidget(this);
  tabs_->setObjectName(QStringLiteral("inspectorTabs"));
  tabs_->setDocumentMode(true);
  v->addWidget(tabs_, 1);

  // ---- Details ----
  QFormLayout* details = addTab(tr("Details"), QStringLiteral("inspectorDetails"));
  title_ = new QLineEdit(this);
  addRow(details, QStringLiteral("title"), title_);
  type_ = new QComboBox(this);
  type_->addItem(tr("Task"), QStringLiteral("task"));
  type_->addItem(tr("Milestone"), QStringLiteral("milestone"));
  addRow(details, QStringLiteral("type"), type_);
  mode_ = new QComboBox(this);
  mode_->addItem(tr("Auto (scheduled)"), QStringLiteral("auto"));
  mode_->addItem(tr("Manual (fixed dates)"), QStringLiteral("manual"));
  addRow(details, QStringLiteral("mode"), mode_);
  status_ = new QComboBox(this);
  status_->addItem(tr("To do"), QStringLiteral("todo"));
  status_->addItem(tr("In progress"), QStringLiteral("in_progress"));
  status_->addItem(tr("Done"), QStringLiteral("done"));
  addRow(details, QStringLiteral("status"), status_);
  percent_ = new QSpinBox(this);
  percent_->setRange(0, 100);
  percent_->setSuffix(QStringLiteral("%"));
  addRow(details, QStringLiteral("percentComplete"), percent_);
  priority_ = new QSpinBox(this);
  priority_->setRange(1, 4);
  priority_->setPrefix(QStringLiteral("P"));
  priority_->setToolTip(tr("1 = highest, 4 = lowest"));
  addRow(details, QStringLiteral("priority"), priority_);
  auto* notesBox = new QWidget(this);
  auto* nv = new QVBoxLayout(notesBox);
  nv->setContentsMargins(0, 0, 0, 0);
  nv->setSpacing(2);
  notes_ = new QPlainTextEdit(notesBox);
  notes_->setTabChangesFocus(true);
  notes_->setPlaceholderText(tr("Markdown"));
  notes_->setMinimumHeight(90);
  notesPreview_ = new QTextBrowser(notesBox);
  notesPreview_->setObjectName(QStringLiteral("notesPreview"));
  notesPreview_->setAccessibleName(tr("Notes preview"));
  notesPreview_->setOpenLinks(false);  // a preview, not a browser
  notesPreview_->setMinimumHeight(90);
  notesPreview_->hide();
  preview_ = new QToolButton(notesBox);
  preview_->setObjectName(QStringLiteral("notesPreviewButton"));
  preview_->setText(tr("Preview"));
  preview_->setAccessibleName(tr("Preview notes"));
  preview_->setCheckable(true);
  connect(preview_, &QToolButton::toggled, this, [this](bool on) {
    if (on) notesPreview_->setMarkdown(notes_->toPlainText());
    notes_->setVisible(!on);
    notesPreview_->setVisible(on);
  });
  nv->addWidget(notes_);
  nv->addWidget(notesPreview_);
  nv->addWidget(preview_, 0, Qt::AlignLeft);
  addRow(details, QStringLiteral("notes"), notesBox);
  fields_.insert(QStringLiteral("notes"), notes_);
  notes_->setObjectName(QStringLiteral("field-notes"));
  notes_->setAccessibleName(taskFieldLabel(QStringLiteral("notes")));
  notesBox->setObjectName(QString());

  // ---- Schedule ----
  QFormLayout* sched = addTab(tr("Schedule"), QStringLiteral("inspectorSchedule"));
  duration_ = new QDoubleSpinBox(this);
  duration_->setRange(0, 100000);
  duration_->setDecimals(2);
  duration_->setSuffix(tr(" days"));
  duration_->setToolTip(tr("Working days; 0 only for a milestone"));
  addRow(sched, QStringLiteral("duration"), duration_);
  addDate(sched, QStringLiteral("start"));
  addDate(sched, QStringLiteral("finish"));
  constraint_ = new QComboBox(this);
  constraint_->addItem(tr("As soon as possible"), QStringLiteral("ASAP"));
  constraint_->addItem(tr("Start no earlier than"), QStringLiteral("SNET"));
  constraint_->addItem(tr("Finish no later than"), QStringLiteral("FNLT"));
  addRow(sched, QStringLiteral("constraint"), constraint_);
  addDate(sched, QStringLiteral("constraintDate"));
  addDate(sched, QStringLiteral("deadline"));
  addDate(sched, QStringLiteral("actualStart"));
  addDate(sched, QStringLiteral("actualFinish"));

  // ---- History ----
  auto* hist = new QWidget(this);
  hist->setObjectName(QStringLiteral("inspectorHistory"));
  auto* hv = new QVBoxLayout(hist);
  hv->setContentsMargins(0, 6, 0, 0);
  historyStatus_ = new QLabel(hist);
  historyStatus_->setObjectName(QStringLiteral("historyStatus"));
  historyStatus_->setWordWrap(true);
  hv->addWidget(historyStatus_);
  history_ = new QTableWidget(0, 3, hist);
  history_->setObjectName(QStringLiteral("historyTable"));
  history_->setAccessibleName(tr("Task history"));
  history_->setHorizontalHeaderLabels({tr("When"), tr("Who"), tr("Change")});
  history_->verticalHeader()->hide();
  history_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  history_->setSelectionBehavior(QAbstractItemView::SelectRows);
  history_->setWordWrap(true);
  history_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  history_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  history_->horizontalHeader()->setStretchLastSection(true);
  hv->addWidget(history_, 1);
  tabs_->addTab(hist, tr("History"));
  connect(tabs_, &QTabWidget::currentChanged, this, [this, hist](int) {
    if (tabs_->currentWidget() == hist && !historyLoaded_) loadHistory();
  });

  message_ = new QLabel(this);
  message_->setObjectName(QStringLiteral("inspectorMessage"));
  message_->setWordWrap(true);
  message_->hide();
  v->addWidget(message_);
  auto* buttons = new QHBoxLayout;
  buttons->addStretch(1);
  revert_ = new QPushButton(tr("Revert"), this);
  revert_->setObjectName(QStringLiteral("inspectorRevert"));
  revert_->setToolTip(tr("Discard your changes and show the server's copy"));
  save_ = new QPushButton(tr("Save"), this);
  save_->setObjectName(QStringLiteral("inspectorSave"));
  save_->setDefault(true);
  buttons->addWidget(revert_);
  buttons->addWidget(save_);
  v->addLayout(buttons);
  connect(save_, &QPushButton::clicked, this, [this] { save(); });
  connect(revert_, &QPushButton::clicked, this, [this] { revert(); });

  // Anything typed: dirty state and the rules between fields.
  for (QLineEdit* l : findChildren<QLineEdit*>()) {
    if (l->objectName().startsWith(QLatin1String("field-"))) connect(l, &QLineEdit::textEdited, this, [this] { edited(); });
  }
  connect(notes_, &QPlainTextEdit::textChanged, this, [this] { edited(); });
  for (QComboBox* c : {type_, mode_, status_, constraint_}) {
    connect(c, &QComboBox::currentIndexChanged, this, [this] { edited(); });
  }
  connect(priority_, &QSpinBox::valueChanged, this, [this] { edited(); });
  connect(duration_, &QDoubleSpinBox::valueChanged, this, [this] { edited(); });
  connect(percent_, &QSpinBox::valueChanged, this, [this](int value) {
    // Work has started: an actual start is required (SPEC.md §5.2), and is
    // the scheduled start unless the user says otherwise.
    if (!filling_ && value > 0 && !isSummary() && text(QStringLiteral("actualStart")).isEmpty()) {
      const QString start = text(QStringLiteral("start"));
      const QString day = start.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : start;
      setText(QStringLiteral("actualStart"), day);
      setMessage(tr("Actual start set to %1 (needed once work has started); change it on the Schedule tab.").arg(day));
    }
    edited();
  });
  connect(type_, &QComboBox::currentIndexChanged, this, [this] {
    if (filling_) return;
    const bool milestone = type_->currentData().toString() == QLatin1String("milestone");
    if (milestone) {
      duration_->setValue(0);
    } else if (duration_->value() == 0.0) {
      duration_->setValue(1);
    }
    updateState();
  });
  connect(constraint_, &QComboBox::currentIndexChanged, this, [this] {
    if (filling_) return;
    if (constraint_->currentData().toString() == QLatin1String("ASAP")) setText(QStringLiteral("constraintDate"), QString());
    updateState();
  });
  connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
    if (!filling_) updateState();
  });
  setMinimumWidth(280);
  updateState();
}

QFormLayout* TaskInspector::addTab(const QString& title, const QString& objectName) {
  auto* scroll = new QScrollArea(this);
  scroll->setObjectName(objectName);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* inner = new QWidget(scroll);
  auto* form = new QFormLayout(inner);
  form->setContentsMargins(0, 6, 4, 0);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  scroll->setWidget(inner);
  tabs_->addTab(scroll, title);
  return form;
}

void TaskInspector::addRow(QFormLayout* form, const QString& name, QWidget* w) {
  w->setObjectName(QStringLiteral("field-") + name);
  w->setAccessibleName(taskFieldLabel(name));
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
  auto* label = new QLabel(taskFieldLabel(name) + QLatin1Char(':'), this);
  label->setBuddy(w);
  form->addRow(label, box);
  fields_.insert(name, w);
  errors_.insert(name, err);
}

QLineEdit* TaskInspector::addDate(QFormLayout* form, const QString& name) {
  auto* e = new QLineEdit(this);
  e->setPlaceholderText(kDate);
  e->setMaxLength(10);
  addRow(form, name, e);
  return e;
}

void TaskInspector::setText(const QString& name, const QString& value) {
  if (auto* l = qobject_cast<QLineEdit*>(fields_.value(name))) l->setText(value);
  if (auto* t = qobject_cast<QPlainTextEdit*>(fields_.value(name))) t->setPlainText(value);
  if (!filling_) edited();
}

QString TaskInspector::text(const QString& name) const {
  if (auto* l = qobject_cast<QLineEdit*>(fields_.value(name))) return l->text().trimmed();
  if (auto* t = qobject_cast<QPlainTextEdit*>(fields_.value(name))) return t->toPlainText();
  return {};
}

QString TaskInspector::taskLabel() const {
  const QString title = base_.value(QStringLiteral("title")).toString();
  return wbs_.isEmpty() ? title : wbs_ + QLatin1Char(' ') + title;
}

// ---- showing a task ----

void TaskInspector::showTask(const QString& id, const QJsonObject& cached, const QString& wbs, int children) {
  const bool same = id == id_;
  wbs_ = wbs;
  children_ = children;
  if (!same) {
    ++gen_;
    id_ = id;
    base_ = cached;
    historyLoaded_ = false;
    history_->setRowCount(0);
    historyStatus_->clear();
    preview_->setChecked(false);
    setMessage(QString());
    clearErrors();
    load(cached);
    fetch();
    if (tabs_->currentIndex() == 2) loadHistory();
  } else if (!isDirty() && !loading_) {
    // Follow the server while untouched: the cache moved on (another
    // computer, an agent, or a roll-up).
    QJsonObject subset;
    for (const QString& k : values().keys()) {
      if (cached.contains(k)) subset.insert(k, cached.value(k));
    }
    if (!client::changedFields(base_, subset).isEmpty()) {
      fetch();
      historyLoaded_ = false;
      if (tabs_->currentIndex() == 2) loadHistory();
    }
  }
  const int kids = children_;
  header_->setText(taskLabel());
  header_->setAccessibleName(tr("Task %1").arg(taskLabel()));
  if (kids > 0) {
    rollupNote_->setText(kids == 1 ? tr("🔒 Summary task: status, % complete and actual dates are rolled up from "
                                        "1 child task.")
                                   : tr("🔒 Summary task: status, % complete and actual dates are rolled up from "
                                        "%1 child tasks.")
                                         .arg(kids));
    rollupNote_->show();
  } else {
    rollupNote_->hide();
  }
  updateState();
}

void TaskInspector::fetch() {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr || !s->editingAllowed() || id_.isEmpty()) {
    loading_ = false;
    updateState();
    return;
  }
  loading_ = true;
  updateState();
  const quint64 gen = gen_;
  QPointer<TaskInspector> self(this);
  s->api()->get(QStringLiteral("/v1/tasks/") + id_, {}, [self, gen](const client::ApiResponse& resp) {
    TaskInspector* me = self.data();
    if (me == nullptr || gen != me->gen_) return;
    me->loading_ = false;
    const client::ApiError e = client::classifyResponse(resp);
    if (!e.ok()) {
      me->setMessage(e.kind == client::ErrorKind::NotFound
                           ? tr("This task was deleted elsewhere.")
                           : tr("Can't read the server's copy, so editing is off: %1").arg(e.message),
                       true);
      me->base_.remove(QStringLiteral("rev"));
      me->updateState();
      return;
    }
    me->base_ = resp.json;
    me->load(me->base_);
    me->header_->setText(me->taskLabel());
    me->updateState();
  });
}

void TaskInspector::reloadFromServer() {
  historyLoaded_ = false;
  fetch();
  if (tabs_->currentIndex() == 2) loadHistory();
}

void TaskInspector::load(const QJsonObject& t) {
  filling_ = true;
  title_->setText(t.value(QStringLiteral("title")).toString());
  auto pick = [](QComboBox* c, const QString& v) { c->setCurrentIndex(std::max(0, c->findData(v))); };
  pick(type_, t.value(QStringLiteral("type")).toString(QStringLiteral("task")));
  pick(mode_, t.value(QStringLiteral("mode")).toString(QStringLiteral("auto")));
  pick(status_, t.value(QStringLiteral("status")).toString(QStringLiteral("todo")));
  pick(constraint_, t.value(QStringLiteral("constraint")).toString(QStringLiteral("ASAP")));
  percent_->setValue(qRound(t.value(QStringLiteral("percentComplete")).toDouble()));
  priority_->setValue(t.value(QStringLiteral("priority")).toInt(2));
  duration_->setValue(t.value(QStringLiteral("duration")).toDouble(1));
  notes_->setPlainText(t.value(QStringLiteral("notes")).toString());
  for (const QString& d : kDates) {
    if (auto* l = qobject_cast<QLineEdit*>(fields_.value(d))) l->setText(t.value(d).toString());
  }
  if (preview_->isChecked()) notesPreview_->setMarkdown(notes_->toPlainText());
  filling_ = false;
  clearErrors();
  edited();
}

void TaskInspector::revert() {
  load(base_);
  setMessage(QString());
}

// ---- state ----

QJsonObject TaskInspector::values() const {
  QJsonObject o;
  o.insert(QStringLiteral("title"), title_->text());
  o.insert(QStringLiteral("type"), type_->currentData().toString());
  o.insert(QStringLiteral("mode"), mode_->currentData().toString());
  o.insert(QStringLiteral("priority"), priority_->value());
  o.insert(QStringLiteral("notes"), notes_->toPlainText());
  o.insert(QStringLiteral("duration"), duration_->value());
  o.insert(QStringLiteral("constraint"), constraint_->currentData().toString());
  for (const char* d : {"start", "finish", "constraintDate", "deadline"}) {
    o.insert(QLatin1String(d), orNull(text(QString::fromLatin1(d))));
  }
  if (!isSummary()) {
    o.insert(QStringLiteral("status"), status_->currentData().toString());
    o.insert(QStringLiteral("percentComplete"), percent_->value());
    o.insert(QStringLiteral("actualStart"), orNull(text(QStringLiteral("actualStart"))));
    o.insert(QStringLiteral("actualFinish"), orNull(text(QStringLiteral("actualFinish"))));
  }
  return o;
}

QJsonObject TaskInspector::body() const {
  return client::changedBody(base_, values());
}

bool TaskInspector::isDirty() const { return !id_.isEmpty() && !client::changedBody(base_, values()).isEmpty(); }

bool TaskInspector::canSave() const {
  return editable_ && !busy_ && !loading_ && base_.value(QStringLiteral("rev")).toInteger() > 0 && isDirty();
}

void TaskInspector::setEditable(bool editable) {
  if (editable_ == editable) return;
  editable_ = editable;
  // Back online with only the cached copy: read the server's.
  if (editable_ && !id_.isEmpty() && base_.value(QStringLiteral("rev")).toInteger() == 0) fetch();
  updateState();
}

void TaskInspector::setBusy(bool busy) {
  busy_ = busy;
  updateState();
}

void TaskInspector::edited() {
  if (filling_) return;
  const bool dirty = isDirty();
  if (dirty != wasDirty_) {
    wasDirty_ = dirty;
    emit dirtyChanged(dirty);
  }
  save_->setEnabled(canSave());
  revert_->setEnabled(dirty && !busy_);
}

void TaskInspector::updateState() {
  const bool live = editable_ && !loading_ && base_.value(QStringLiteral("rev")).toInteger() > 0;
  const bool milestone = type_->currentData().toString() == QLatin1String("milestone");
  const bool manual = mode_->currentData().toString() == QLatin1String("manual");
  const bool asap = constraint_->currentData().toString() == QLatin1String("ASAP");
  const QString rolled =
      children_ == 1 ? tr("Rolled up from 1 child task") : tr("Rolled up from %1 child tasks").arg(children_);
  for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
    const QString& k = it.key();
    bool ro = !live;
    QString tip;
    if (isSummary() && kRolledUp.contains(k)) {
      ro = true;
      tip = rolled;
    } else if (k == QLatin1String("type") && isSummary()) {
      ro = true;
      tip = tr("A task with subtasks can't be a milestone");
    } else if (k == QLatin1String("duration") && milestone) {
      ro = true;
      tip = tr("A milestone has no duration");
    } else if ((k == QLatin1String("start") || k == QLatin1String("finish")) && !manual) {
      ro = true;
      tip = tr("Auto tasks get their dates from the schedule; switch Mode to Manual to set them");
    } else if (k == QLatin1String("constraintDate") && asap) {
      ro = true;
      tip = tr("No date for As soon as possible");
    }
    setReadOnly(it.value(), ro);
    it.value()->setToolTip(tip);
    it.value()->setAccessibleDescription(tip.isEmpty() ? QString() : tip);
  }
  if (loading_) {
    if (message_->text().isEmpty()) setMessage(tr("Reading the server's copy…"));
  } else if (!editable_ && !id_.isEmpty()) {
    setMessage(tr("Read-only: editing is off until the server answers."));
  } else if (message_->text() == tr("Reading the server's copy…") ||
             message_->text() == tr("Read-only: editing is off until the server answers.")) {
    setMessage(QString());
  }
  save_->setEnabled(canSave());
  revert_->setEnabled(isDirty() && !busy_);
}

// ---- checks ----

bool TaskInspector::validate() {
  clearErrors();
  const QList<FieldError> errs = client::validateTaskEdit(base_, values(), isSummary());
  for (const FieldError& e : errs) showFieldError(e.field, e.message);
  if (!errs.isEmpty()) {
    setMessage(tr("Fix the fields marked ⚠ and save again. Nothing was sent."), true);
    // Show the tab with the first problem.
    if (QWidget* w = fields_.value(errs.first().field)) {
      for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->widget(i)->isAncestorOf(w)) tabs_->setCurrentIndex(i);
      }
      w->setFocus();
    }
  }
  return errs.isEmpty();
}

bool TaskInspector::save() {
  if (!canSave()) return false;
  if (!validate()) return false;
  emit saveRequested(id_, base_, body());
  return true;
}

void TaskInspector::showFieldError(const QString& field, const QString& message) {
  QLabel* l = errors_.value(field);
  if (l == nullptr) {
    setMessage(QStringLiteral("⚠ %1: %2").arg(taskFieldLabel(field), message), true);
    return;
  }
  l->setText(QStringLiteral("⚠ %1 %2").arg(taskFieldLabel(field), message));
  l->setAccessibleName(l->text());
  l->show();
  if (QWidget* w = fields_.value(field)) w->setAccessibleDescription(l->text());
}

void TaskInspector::clearErrors() {
  for (auto it = errors_.constBegin(); it != errors_.constEnd(); ++it) {
    it.value()->clear();
    it.value()->hide();
  }
  if (message_->text().startsWith(QStringLiteral("⚠ "))) setMessage(QString());
}

void TaskInspector::setMessage(const QString& text, bool error) {
  message_->setText(error && !text.isEmpty() ? QStringLiteral("⚠ ") + text : text);
  message_->setAccessibleName(message_->text());
  message_->setVisible(!text.isEmpty());
}

// ---- history ----

QString TaskInspector::who(const QString& actor) const {
  AppSession* s = session_();
  return changeActorName(actor, s != nullptr ? s->connection().tokenLabel : QString());
}

void TaskInspector::loadHistory() {
  historyLoaded_ = true;
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr || id_.isEmpty()) {
    historyStatus_->setText(tr("History needs the server."));
    return;
  }
  historyStatus_->setText(tr("Loading history…"));
  fetchHistory(0, 0, {}, gen_);
}

void TaskInspector::fetchHistory(qint64 since, int page, QList<QJsonObject> rows, quint64 gen) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr) return;
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("since"), QString::number(since));
  q.addQueryItem(QStringLiteral("limit"), QString::number(kHistoryPage));
  QPointer<TaskInspector> self(this);
  s->api()->get(QStringLiteral("/v1/tasks/%1/history").arg(id_), q,
                [self, page, rows, gen](const client::ApiResponse& resp) mutable {
                  TaskInspector* me = self.data();
                  if (me == nullptr || gen != me->gen_) return;
                  const client::ApiError e = client::classifyResponse(resp);
                  if (!e.ok()) {
                    me->historyLoaded_ = false;
                    me->historyStatus_->setText(tr("Can't load the history: %1").arg(e.message));
                    return;
                  }
                  const QJsonArray items = resp.json.value(QStringLiteral("items")).toArray();
                  qint64 last = 0;
                  for (const QJsonValue& v : items) {
                    rows.append(v.toObject());
                    last = v.toObject().value(QStringLiteral("seq")).toInteger();
                  }
                  if (items.size() == kHistoryPage && page + 1 < kHistoryPages) {
                    me->fetchHistory(last, page + 1, rows, gen);
                    return;
                  }
                  me->fillHistory(rows);
                  if (items.size() == kHistoryPage) {
                    me->historyStatus_->setText(tr("Showing the first %1 changes.").arg(rows.size()));
                  }
                });
}

void TaskInspector::fillHistory(const QList<QJsonObject>& rows) {
  history_->setRowCount(0);
  history_->setRowCount(static_cast<int>(rows.size()));
  int r = 0;
  for (auto it = rows.crbegin(); it != rows.crend(); ++it, ++r) {  // newest first
    const QJsonObject& c = *it;
    const QString when = changeLocalTime(c.value(QStringLiteral("ts")).toString());
    const QString by = who(c.value(QStringLiteral("actor")).toString());
    const QString what = describeChange(QStringLiteral("task"), c.value(QStringLiteral("before")).toObject(), c.value(QStringLiteral("after")).toObject());
    auto* a = new QTableWidgetItem(when);
    auto* b = new QTableWidgetItem(by);
    const QString actor = c.value(QStringLiteral("actor")).toString();
    b->setToolTip(changeActorLabel(actor));
    auto* d = new QTableWidgetItem(what);
    d->setToolTip(what);
    for (QTableWidgetItem* i : {a, b, d}) i->setData(Qt::AccessibleTextRole, i->text());
    history_->setItem(r, 0, a);
    history_->setItem(r, 1, b);
    history_->setItem(r, 2, d);
  }
  history_->resizeRowsToContents();
  historyStatus_->setText(rows.isEmpty()      ? tr("No changes yet.")
                          : rows.size() == 1 ? tr("1 change.")
                                             : tr("%1 changes, newest first.").arg(rows.size()));
}

}  // namespace zproject::app
