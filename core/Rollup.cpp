#include "Rollup.hpp"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>

namespace zproject {
namespace {

const QString kTodo = QStringLiteral("todo");
const QString kInProgress = QStringLiteral("in_progress");
const QString kDone = QStringLiteral("done");

// What a subtree contributes to its parent.
struct Acc {
  double weight = 0;        // Σ duration over leaves
  double weighted = 0;      // Σ percent × duration over leaves
  double plainSum = 0;      // Σ percent over leaves
  qsizetype leaves = 0;
  bool allLeavesComplete = true;  // every leaf at 100 %
};

}  // namespace

QSet<QString> summaryIds(const QList<Task>& tasks) {
  QSet<QString> live;
  for (const Task& t : tasks) {
    if (!t.deleted) live.insert(t.id);
  }
  QSet<QString> out;
  for (const Task& t : tasks) {
    if (!t.deleted && !t.parent.isEmpty() && live.contains(t.parent)) out.insert(t.parent);
  }
  return out;
}

QList<qsizetype> rollUpSummaries(QList<Task>& tasks) {
  QHash<QString, qsizetype> index;
  for (qsizetype i = 0; i < tasks.size(); ++i) {
    if (!tasks[i].deleted) index.insert(tasks[i].id, i);
  }
  QHash<QString, QList<qsizetype>> children;
  QList<qsizetype> roots;
  for (qsizetype i = 0; i < tasks.size(); ++i) {
    const Task& t = tasks[i];
    if (t.deleted) continue;
    if (!t.parent.isEmpty() && index.contains(t.parent)) {
      children[t.parent].append(i);
    } else {
      roots.append(i);
    }
  }

  QList<qsizetype> changed;
  std::function<Acc(qsizetype)> visit = [&](qsizetype i) -> Acc {
    const QList<qsizetype> kids = children.value(tasks[i].id);
    if (kids.isEmpty()) {
      const Task& t = tasks[i];
      Acc a;
      a.weight = t.duration;
      a.weighted = t.percentComplete * t.duration;
      a.plainSum = t.percentComplete;
      a.leaves = 1;
      a.allLeavesComplete = t.percentComplete >= 100;
      return a;
    }
    Acc sum;
    bool allDone = true;
    bool allTodo = true;
    bool allFinished = true;
    QString start;
    QString finish;
    for (qsizetype k : kids) {
      const Acc a = visit(k);
      sum.weight += a.weight;
      sum.weighted += a.weighted;
      sum.plainSum += a.plainSum;
      sum.leaves += a.leaves;
      sum.allLeavesComplete = sum.allLeavesComplete && a.allLeavesComplete;
      const Task& c = tasks[k];  // already rolled up if it is a summary
      allDone = allDone && c.status == kDone;
      allTodo = allTodo && c.status == kTodo;
      if (!c.actualStart.isEmpty() && (start.isEmpty() || c.actualStart < start)) start = c.actualStart;
      if (c.actualFinish.isEmpty()) {
        allFinished = false;
      } else if (finish.isEmpty() || c.actualFinish > finish) {
        finish = c.actualFinish;
      }
    }

    int percent = 0;
    if (sum.weight > 0) {
      const double exact = sum.weighted / sum.weight;
      percent = static_cast<int>(std::lround(exact));
      if (percent == 0 && sum.weighted > 0) percent = 1;
    } else if (sum.leaves > 0) {
      percent = static_cast<int>(std::lround(sum.plainSum / static_cast<double>(sum.leaves)));
    }
    percent = std::clamp(percent, 0, 100);
    if (sum.allLeavesComplete) {
      percent = 100;
    } else if (percent == 100) {
      percent = 99;
    }

    Task& t = tasks[i];
    const QString status = allDone ? kDone : allTodo ? kTodo : kInProgress;
    const QString actualFinish = allFinished ? finish : QString();
    // validateTask: actualFinish implies 100 %; percent > 0 implies actualStart.
    if (t.percentComplete != percent || t.status != status || t.actualStart != start ||
        t.actualFinish != actualFinish) {
      t.percentComplete = percent;
      t.status = status;
      t.actualStart = start;
      t.actualFinish = actualFinish;
      changed.append(i);
    }
    return sum;
  };
  for (qsizetype r : roots) visit(r);
  return changed;
}

QList<FieldError> checkSummaryEdit(const Task& current, const Task& next) {
  QList<FieldError> errs;
  const QString msg = QStringLiteral("is derived from the child tasks of a summary task and cannot be set");
  if (next.percentComplete != current.percentComplete) errs.append({QStringLiteral("percentComplete"), msg});
  if (next.status != current.status) errs.append({QStringLiteral("status"), msg});
  if (next.actualStart != current.actualStart) errs.append({QStringLiteral("actualStart"), msg});
  if (next.actualFinish != current.actualFinish) errs.append({QStringLiteral("actualFinish"), msg});
  return errs;
}

}  // namespace zproject
