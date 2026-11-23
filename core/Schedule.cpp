#include "core/Schedule.hpp"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <vector>

namespace zproject {

namespace {

constexpr qint64 kNone = std::numeric_limits<qint64>::min();

// A point in time at the start or the end of a day.
struct Moment {
  QDate date;
  bool end = false;
};
bool before(const Moment& a, const Moment& b) {
  if (a.date != b.date) return a.date < b.date;
  return !a.end && b.end;
}

// One calendar seen as a line of working days counted from a shared origin:
// boundary p is the start of the p-th working day (0-based) on or after the
// origin, which is also the end of the one before it.
struct Line {
  const WorkCalendar* cal = nullptr;
  QDate origin;

  qint64 pos(const Moment& m) const {
    qint64 p = cal->workingDaysBetween(origin, m.date);
    if (m.end && cal->isWorking(m.date)) ++p;
    return p;
  }
  // The working day whose start is boundary p.
  QDate dayAt(qint64 p) const {
    return p >= 0 ? cal->addWorkingDays(origin.addDays(-1), static_cast<int>(p + 1))
                  : cal->addWorkingDays(origin, static_cast<int>(p));
  }
  Moment startOf(qint64 p) const { return {dayAt(p), false}; }
};

QDate date(const QString& iso) { return QDate::fromString(iso, Qt::ISODate); }

struct Node {
  int index = 0;
  const Task* task = nullptr;
  int parent = -1;
  std::vector<int> children;
  std::vector<int> leaves;  // summaries: leaf descendants, in input order
  std::vector<int> in;      // links whose successor is this task
  std::vector<int> out;     // links whose predecessor is this task
  bool summary() const { return !children.empty(); }

  // Leaves.
  Line line;
  qint64 dur = 0;
  bool milestone = false;
  bool fixed = false;
  bool done = false;
  bool scheduled = false;
  bool msEnd = false;  // a milestone driven by a finish: shown on that day
  qint64 es = 0;
  qint64 ef = 0;
  qint64 ls = 0;
  qint64 lf = 0;
  Moment sm;
  Moment fm;
};

qint64 workingDays(double duration) {
  if (duration <= 0) return 0;
  return std::max<qint64>(1, static_cast<qint64>(std::ceil(duration - 1e-9)));
}

}  // namespace

Result<Schedule> computeSchedule(const ScheduleInput& in) {
  Schedule out;
  const int n = static_cast<int>(in.tasks.size());
  std::vector<Node> nodes(static_cast<size_t>(n));
  QHash<QString, int> byId;
  for (int i = 0; i < n; ++i) {
    nodes[static_cast<size_t>(i)].index = i;
    nodes[static_cast<size_t>(i)].task = &in.tasks.at(i);
    byId.insert(in.tasks.at(i).id, i);
    out.order << in.tasks.at(i).id;
  }
  for (Node& nd : nodes) {
    const auto p = byId.constFind(nd.task->parent);
    if (!nd.task->parent.isEmpty() && p != byId.constEnd() && p.value() != nd.index) {
      nd.parent = p.value();
      nodes[static_cast<size_t>(nd.parent)].children.push_back(nd.index);
    }
  }
  // A parent loop in the input (never valid) would make the leaf walk spin.
  for (const Node& nd : nodes) {
    int steps = 0;
    for (int a = nd.parent; a >= 0; a = nodes[static_cast<size_t>(a)].parent) {
      if (++steps > n) return Error::cycle(QStringLiteral("task %1 is its own ancestor").arg(nd.task->id));
    }
  }
  std::function<void(int, std::vector<int>&)> collect = [&](int i, std::vector<int>& acc) {
    const Node& nd = nodes[static_cast<size_t>(i)];
    if (!nd.summary()) {
      acc.push_back(i);
      return;
    }
    for (int c : nd.children) collect(c, acc);
  };
  for (Node& nd : nodes) {
    if (nd.summary()) {
      collect(nd.index, nd.leaves);
      std::sort(nd.leaves.begin(), nd.leaves.end());
    }
  }
  const int m = static_cast<int>(in.links.size());
  std::vector<std::pair<int, int>> ends(static_cast<size_t>(m), {-1, -1});
  for (int k = 0; k < m; ++k) {
    const Dependency& l = in.links.at(k);
    const auto p = byId.constFind(l.predecessor);
    const auto s = byId.constFind(l.successor);
    if (p == byId.constEnd() || s == byId.constEnd()) continue;  // a link to a task not in this set
    ends[static_cast<size_t>(k)] = {p.value(), s.value()};
    nodes[static_cast<size_t>(p.value())].out.push_back(k);
    nodes[static_cast<size_t>(s.value())].in.push_back(k);
  }
  auto targets = [&](int i) -> std::vector<int> {
    const Node& nd = nodes[static_cast<size_t>(i)];
    return nd.summary() ? nd.leaves : std::vector<int>{i};
  };

  // ---- order of work (Kahn, ties by input order) ----
  std::vector<std::vector<int>> edges(static_cast<size_t>(n));
  std::vector<int> indegree(static_cast<size_t>(n), 0);
  auto edge = [&](int a, int b) {
    edges[static_cast<size_t>(a)].push_back(b);
    ++indegree[static_cast<size_t>(b)];
  };
  for (const Node& nd : nodes) {
    if (nd.parent >= 0) edge(nd.index, nd.parent);  // a summary's span needs its children
  }
  for (int k = 0; k < m; ++k) {
    const auto [p, s] = ends[static_cast<size_t>(k)];
    if (p < 0) continue;
    for (int t : targets(s)) edge(p, t);
  }
  std::priority_queue<int, std::vector<int>, std::greater<>> ready;
  for (int i = 0; i < n; ++i) {
    if (indegree[static_cast<size_t>(i)] == 0) ready.push(i);
  }
  std::vector<int> topo;
  topo.reserve(static_cast<size_t>(n));
  while (!ready.empty()) {
    const int i = ready.top();
    ready.pop();
    topo.push_back(i);
    for (int j : edges[static_cast<size_t>(i)]) {
      if (--indegree[static_cast<size_t>(j)] == 0) ready.push(j);
    }
  }
  if (static_cast<int>(topo.size()) != n) {
    // Walk forward among the tasks left until one repeats: that is a cycle.
    int start = 0;
    while (indegree[static_cast<size_t>(start)] == 0) ++start;
    std::vector<int> seenAt(static_cast<size_t>(n), -1);
    std::vector<int> path;
    int cur = start;
    while (seenAt[static_cast<size_t>(cur)] < 0) {
      seenAt[static_cast<size_t>(cur)] = static_cast<int>(path.size());
      path.push_back(cur);
      for (int j : edges[static_cast<size_t>(cur)]) {
        if (indegree[static_cast<size_t>(j)] > 0) {
          cur = j;
          break;
        }
      }
    }
    QJsonArray ids;
    QStringList titles;
    for (size_t i = static_cast<size_t>(seenAt[static_cast<size_t>(cur)]); i < path.size(); ++i) {
      ids.append(in.tasks.at(path[i]).id);
      titles << in.tasks.at(path[i]).title;
    }
    ids.append(in.tasks.at(cur).id);
    titles << in.tasks.at(cur).title;
    return Error::cycle(QStringLiteral("the links make a loop: %1").arg(titles.join(QStringLiteral(" → "))),
                        QJsonObject{{QStringLiteral("path"), ids}});
  }

  // ---- calendars ----
  QHash<QString, WorkCalendar> cals = in.calendars;
  const WorkCalendar standard;
  QDate origin = in.projectStart;
  if (!origin.isValid()) {
    for (const Task& t : in.tasks) {
      for (const QString& s : {t.start, t.finish, t.actualStart, t.actualFinish, t.constraintDate}) {
        const QDate d = date(s);
        if (d.isValid() && (!origin.isValid() || d < origin)) origin = d;
      }
    }
  }
  if (!origin.isValid()) origin = QDate(2000, 1, 3);
  const QDate lo = origin.addDays(-400);
  const QDate hi = origin.addDays(qint64{15} * 366);
  for (auto it = cals.begin(); it != cals.end(); ++it) it->prepare(lo, hi);
  WorkCalendar preparedStandard = standard;
  preparedStandard.prepare(lo, hi);
  auto violation = [&](const QString& kind, int task, const QString& other, const QString& message) {
    out.violations.append({kind, task >= 0 ? in.tasks.at(task).id : QString(), other, message});
  };
  const WorkCalendar* projectCal = &preparedStandard;
  if (!in.projectCalendar.isEmpty()) {
    const auto it = cals.constFind(in.projectCalendar);
    if (it != cals.constEnd()) {
      projectCal = &it.value();
    } else {
      violation(QStringLiteral("unknown_calendar"), -1, in.projectCalendar,
                QStringLiteral("the project's calendar %1 is unknown; Standard is used").arg(in.projectCalendar));
    }
  }
  for (Node& nd : nodes) {
    const WorkCalendar* c = projectCal;
    if (!nd.task->calendar.isEmpty()) {
      const auto it = cals.constFind(nd.task->calendar);
      if (it != cals.constEnd()) {
        c = &it.value();
      } else {
        violation(QStringLiteral("unknown_calendar"), nd.index, nd.task->calendar,
                  QStringLiteral("calendar %1 is unknown; the project's is used").arg(nd.task->calendar));
      }
    }
    nd.line = {c, origin};
    nd.milestone = nd.task->type == QLatin1String("milestone");
    nd.dur = nd.milestone ? 0 : workingDays(nd.task->duration);
    nd.done = date(nd.task->actualFinish).isValid();
  }

  // Requirements on a leaf: its own incoming links and its ancestors'.
  auto incoming = [&](int i, const std::function<void(int)>& f) {
    for (int a = i; a >= 0; a = nodes[static_cast<size_t>(a)].parent) {
      for (int k : nodes[static_cast<size_t>(a)].in) f(k);
    }
  };
  auto outgoing = [&](int i, const std::function<void(int)>& f) {
    for (int a = i; a >= 0; a = nodes[static_cast<size_t>(a)].parent) {
      for (int k : nodes[static_cast<size_t>(a)].out) f(k);
    }
  };
  // A task's (or summary's) start and finish moments, for its successors.
  auto startMoment = [&](int i, Moment* mo) {
    const Node& nd = nodes[static_cast<size_t>(i)];
    if (!nd.summary()) {
      if (nd.scheduled) *mo = nd.sm;
      return nd.scheduled;
    }
    bool any = false;
    for (int l : nd.leaves) {
      const Node& x = nodes[static_cast<size_t>(l)];
      if (x.scheduled && (!any || before(x.sm, *mo))) *mo = x.sm;
      any = any || x.scheduled;
    }
    return any;
  };
  auto finishMoment = [&](int i, Moment* mo) {
    const Node& nd = nodes[static_cast<size_t>(i)];
    if (!nd.summary()) {
      if (nd.scheduled) *mo = nd.fm;
      return nd.scheduled;
    }
    bool any = false;
    for (int l : nd.leaves) {
      const Node& x = nodes[static_cast<size_t>(l)];
      if (x.scheduled && (!any || before(*mo, x.fm))) *mo = x.fm;
      any = any || x.scheduled;
    }
    return any;
  };
  auto setMoments = [](Node& nd) {
    if (nd.dur > 0) {
      nd.sm = {nd.line.dayAt(nd.es), false};
      nd.fm = {nd.line.dayAt(nd.ef - 1), true};
    } else if (nd.msEnd) {
      nd.sm = nd.fm = {nd.line.dayAt(nd.es - 1), true};
    } else {
      nd.sm = nd.fm = {nd.line.dayAt(nd.es), false};
    }
  };

  // ---- forward pass ----
  for (int i : topo) {
    Node& nd = nodes[static_cast<size_t>(i)];
    if (nd.summary()) continue;
    const Task& t = *nd.task;
    // What the links ask for: the latest start (and finish, for FF).
    qint64 needStart = kNone;
    qint64 needFinish = kNone;
    bool needEnd = false;  // the binding requirement is a finish
    QString needFrom;
    QString needFinishFrom;
    incoming(i, [&](int k) {
      const Dependency& l = in.links.at(k);
      const int p = ends[static_cast<size_t>(k)].first;
      Moment mo;
      if (l.type == QLatin1String("SS")) {
        if (!startMoment(p, &mo)) return;
        const qint64 v = nd.line.pos(mo) + l.lag;
        if (v > needStart || (v == needStart && !needEnd)) {
          if (v > needStart) needEnd = false;
          needStart = v;
          needFrom = l.predecessor;
        }
        return;
      }
      if (!finishMoment(p, &mo)) return;
      const qint64 v = nd.line.pos(mo) + l.lag;
      if (l.type == QLatin1String("FF")) {
        if (v > needFinish) {
          needFinish = v;
          needFinishFrom = l.predecessor;
        }
        if (v - nd.dur >= needStart) {
          needStart = v - nd.dur;
          needEnd = true;
          needFrom = l.predecessor;
        }
      } else if (v >= needStart) {  // FS
        needStart = v;
        needEnd = true;
        needFrom = l.predecessor;
      }
    });

    if (t.mode == QLatin1String("manual")) {
      const QDate s = date(t.start);
      const QDate f = date(t.finish);
      if (!s.isValid() || !f.isValid() || f < s) {
        violation(QStringLiteral("manual_no_dates"), i, {}, QStringLiteral("a manual task needs a start and a finish"));
        continue;
      }
      nd.fixed = true;
      nd.scheduled = true;
      if (nd.milestone) {
        nd.sm = nd.fm = {s, true};
        nd.msEnd = true;
      } else {
        nd.sm = {s, false};
        nd.fm = {f, true};
      }
      nd.es = nd.line.pos(nd.sm);
      nd.ef = nd.line.pos(nd.fm);
      if (needStart != kNone && nd.es < needStart) {
        violation(QStringLiteral("link_broken"), i, needFrom,
                  QStringLiteral("starts before its link from %1 allows").arg(needFrom));
      } else if (needFinish != kNone && nd.ef < needFinish) {
        violation(QStringLiteral("link_broken"), i, needFinishFrom,
                  QStringLiteral("finishes before its link from %1 allows").arg(needFinishFrom));
      }
      continue;
    }

    const QDate as = date(t.actualStart);
    const QDate af = date(t.actualFinish);
    if (af.isValid()) {
      // Done: exactly as it happened.
      nd.fixed = true;
      nd.scheduled = true;
      nd.fm = {af, true};
      nd.sm = nd.milestone ? nd.fm : Moment{as.isValid() ? as : af, false};
      nd.msEnd = true;
      nd.es = nd.line.pos(nd.sm);
      nd.ef = nd.line.pos(nd.fm);
      continue;
    }
    if (as.isValid()) {
      nd.es = nd.line.pos({as, false});
      nd.msEnd = false;
    } else {
      qint64 es = kNone;
      bool atEnd = false;
      if (in.projectStart.isValid()) es = nd.line.pos({in.projectStart, false});
      if (t.constraintType == QLatin1String("SNET")) {
        const QDate c = date(t.constraintDate);
        if (c.isValid() && nd.line.pos({c, false}) > es) es = nd.line.pos({c, false});
      }
      if (needStart != kNone && (needStart > es || (needStart == es && needEnd))) {
        es = needStart;
        atEnd = needEnd;
      }
      if (es == kNone) {
        violation(QStringLiteral("no_start"), i, {},
                  QStringLiteral("nothing sets its start: give the project a start date, or the task a constraint or a "
                                 "predecessor"));
        continue;
      }
      nd.es = es;
      nd.msEnd = nd.milestone && atEnd;
    }
    nd.ef = nd.es + nd.dur;
    nd.scheduled = true;
    setMoments(nd);
  }

  // ---- project finish ----
  Moment finish;
  bool anyScheduled = false;
  for (const Node& nd : nodes) {
    if (nd.summary() || !nd.scheduled) continue;
    if (!anyScheduled || before(finish, nd.fm)) finish = nd.fm;
    anyScheduled = true;
  }
  if (anyScheduled) out.finish = finish.date;
  Moment horizon = finish;
  if (in.projectFinish.isValid() && (!anyScheduled || before(horizon, Moment{in.projectFinish, true}))) {
    horizon = {in.projectFinish, true};
  }

  // ---- backward pass ----
  for (auto it = topo.crbegin(); it != topo.crend(); ++it) {
    Node& nd = nodes[static_cast<size_t>(*it)];
    if (nd.summary() || !nd.scheduled) continue;
    const qint64 span = nd.ef - nd.es;
    qint64 lf = nd.line.pos(horizon);
    if (nd.task->constraintType == QLatin1String("FNLT")) {
      const QDate c = date(nd.task->constraintDate);
      if (c.isValid()) lf = std::min(lf, nd.line.pos({c, true}));
    }
    outgoing(nd.index, [&](int k) {
      const Dependency& l = in.links.at(k);
      for (int x : targets(ends[static_cast<size_t>(k)].second)) {
        const Node& s = nodes[static_cast<size_t>(x)];
        if (!s.scheduled) continue;
        if (l.type == QLatin1String("FF")) {
          lf = std::min(lf, nd.line.pos(s.line.startOf(s.lf - l.lag)));
        } else if (l.type == QLatin1String("SS")) {
          lf = std::min(lf, nd.line.pos(s.line.startOf(s.ls - l.lag)) + span);
        } else {
          lf = std::min(lf, nd.line.pos(s.line.startOf(s.ls - l.lag)));
        }
      }
    });
    nd.lf = lf;
    nd.ls = lf - span;
  }

  // ---- results ----
  for (Node& nd : nodes) {
    ScheduledTask r;
    r.id = nd.task->id;
    r.milestone = nd.milestone;
    if (!nd.summary()) {
      r.scheduled = nd.scheduled;
      r.fixed = nd.fixed;
      if (nd.scheduled) {
        r.start = nd.sm.date;
        r.finish = nd.fm.date;
        if (nd.dur > 0 || (nd.fixed && nd.ef > nd.es)) {
          r.lateStart = nd.line.dayAt(nd.ls);
          r.lateFinish = nd.line.dayAt(nd.lf - 1);
        } else {
          r.lateStart = r.lateFinish = nd.msEnd ? nd.line.dayAt(nd.lf - 1) : nd.line.dayAt(nd.lf);
        }
        r.totalFloat = nd.lf - nd.ef;
        r.critical = !nd.done && r.totalFloat <= 0;
        // Free float: the room before the nearest successor moves.
        qint64 free = kNone;
        outgoing(nd.index, [&](int k) {
          const Dependency& l = in.links.at(k);
          for (int x : targets(ends[static_cast<size_t>(k)].second)) {
            const Node& s = nodes[static_cast<size_t>(x)];
            if (!s.scheduled) continue;
            qint64 slack = 0;
            if (l.type == QLatin1String("SS")) {
              slack = s.es - (s.line.pos(nd.sm) + l.lag);
            } else if (l.type == QLatin1String("FF")) {
              slack = s.ef - (s.line.pos(nd.fm) + l.lag);
            } else {
              slack = s.es - (s.line.pos(nd.fm) + l.lag);
            }
            if (free == kNone || slack < free) free = slack;
          }
        });
        // Never more than the total float (a task without successors has its total).
        r.freeFloat = free == kNone ? r.totalFloat : std::min(free, r.totalFloat);
        const QDate fnlt = nd.task->constraintType == QLatin1String("FNLT") ? date(nd.task->constraintDate) : QDate();
        if (fnlt.isValid() && nd.ef > nd.line.pos({fnlt, true})) {
          violation(QStringLiteral("constraint_missed"), nd.index, {},
                    QStringLiteral("finishes after its finish-no-later-than date %1").arg(nd.task->constraintDate));
        }
      }
    } else {
      r.summary = true;
      r.scheduled = std::any_of(nd.leaves.begin(), nd.leaves.end(),
                                [&](int l) { return nodes[static_cast<size_t>(l)].scheduled; });
    }
    out.tasks.insert(r.id, r);
  }
  // Summaries from their scheduled leaves (all leaf rows exist now).
  for (const Node& nd : nodes) {
    if (!nd.summary()) continue;
    ScheduledTask& r = out.tasks[nd.task->id];
    bool first = true;
    for (int l : nd.leaves) {
      const ScheduledTask& c = out.tasks[nodes[static_cast<size_t>(l)].task->id];
      if (!c.scheduled) continue;
      if (first || c.start < r.start) r.start = c.start;
      if (first || c.finish > r.finish) r.finish = c.finish;
      if (first || c.lateStart < r.lateStart) r.lateStart = c.lateStart;
      if (first || c.lateFinish > r.lateFinish) r.lateFinish = c.lateFinish;
      if (first || c.totalFloat < r.totalFloat) r.totalFloat = c.totalFloat;
      if (first || c.freeFloat < r.freeFloat) r.freeFloat = c.freeFloat;
      r.critical = r.critical || c.critical;
      first = false;
    }
  }
  // Deadlines (reported only), on leaves and summaries.
  for (const Node& nd : nodes) {
    const QDate deadline = date(nd.task->deadline);
    const ScheduledTask& r = out.tasks[nd.task->id];
    if (deadline.isValid() && r.scheduled && r.finish > deadline) {
      violation(QStringLiteral("deadline_missed"), nd.index, {},
                QStringLiteral("finishes %1, after its deadline %2").arg(r.finish.toString(Qt::ISODate), nd.task->deadline));
    }
  }
  // Critical leaves by start, then input order.
  std::vector<int> crit;
  for (const Node& nd : nodes) {
    if (!nd.summary() && out.tasks[nd.task->id].critical) crit.push_back(nd.index);
  }
  std::stable_sort(crit.begin(), crit.end(), [&](int a, int b) {
    const Node& x = nodes[static_cast<size_t>(a)];
    const Node& y = nodes[static_cast<size_t>(b)];
    if (before(x.sm, y.sm)) return true;
    if (before(y.sm, x.sm)) return false;
    return a < b;
  });
  for (int i : crit) out.critical << in.tasks.at(i).id;
  // Violations in input order of their task (project-level first).
  std::stable_sort(out.violations.begin(), out.violations.end(),
                   [&](const ScheduleViolation& a, const ScheduleViolation& b) {
                     return byId.value(a.task, -1) < byId.value(b.task, -1);
                   });
  return out;
}

Ripple diffSchedules(const Schedule& before, const Schedule& after) {
  Ripple r;
  for (const QString& id : after.order) {
    const ScheduledTask* a = after.task(id);
    const ScheduledTask* b = before.task(id);
    if (a == nullptr || b == nullptr || a->summary) continue;
    if (a->start == b->start && a->finish == b->finish) continue;
    RippleMove mv;
    mv.task = id;
    mv.startBefore = b->start;
    mv.startAfter = a->start;
    mv.finishBefore = b->finish;
    mv.finishAfter = a->finish;
    if (b->start.isValid() && a->start.isValid()) mv.startDelta = b->start.daysTo(a->start);
    if (b->finish.isValid() && a->finish.isValid()) mv.finishDelta = b->finish.daysTo(a->finish);
    r.moved.append(mv);
  }
  if (before.finish.isValid() && after.finish.isValid()) r.finishDelta = before.finish.daysTo(after.finish);
  r.criticalChanged = QSet<QString>(before.critical.cbegin(), before.critical.cend()) !=
                      QSet<QString>(after.critical.cbegin(), after.critical.cend());
  return r;
}

}  // namespace zproject
