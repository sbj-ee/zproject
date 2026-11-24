// Desktop exports and printing (SPEC.md §7.9, §8.1):
//  - ProjectPrintout on its own: pagination, the timescale, bars on the
//    page, a PDF with one page per laid-out page, an empty project;
//  - File → Export…, Export as PDF… and Print… in the window, offscreen,
//    against the real zprojectd API on loopback: every file kind from the
//    server's snapshot (with links), the printer, canceling, and offline
//    exports from the cache (asked first, without links).
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QSignalSpy>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>

#include "app/ExportActions.hpp"
#include "app/MainWindow.hpp"
#include "app/ProjectPrint.hpp"
#include "client/TokenStore.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;
using Kind = ExportActions::Kind;

namespace {

const store::WriteContext kOther{QStringLiteral("tok_robin"), QStringLiteral("req-other")};

Task task(const QString& id, const QString& title, qint64 order, const QString& parent, const QString& start,
          const QString& finish, double duration = 3) {
  Task t;
  t.id = id;
  t.title = title;
  t.order = order;
  t.parent = parent;
  t.mode = QStringLiteral("manual");
  t.start = start;
  t.finish = finish;
  t.duration = duration;
  return t;
}

// n leaf tasks under a summary, a week apart from 2027-01-04.
ExportData plan(int n, int daysApart = 7) {
  ExportData d;
  d.project.name = QStringLiteral("Office move");
  d.project.owner = QStringLiteral("Sam");
  d.tasks.append(task(QStringLiteral("s"), QStringLiteral("Phase one"), 0, {}, {}, {}));
  const QDate start(2027, 1, 4);
  for (int i = 0; i < n; ++i) {
    const QDate s = start.addDays(static_cast<qint64>(i) * daysApart);
    Task t = task(QStringLiteral("t%1").arg(i), QStringLiteral("Task %1").arg(i), i, QStringLiteral("s"),
                  s.toString(Qt::ISODate), s.addDays(2).toString(Qt::ISODate));
    t.mode = QStringLiteral("auto");  // solid bars (manual ones are outlined)
    t.percentComplete = 50;
    d.tasks.append(t);
  }
  return d;
}

QColor barColor() { return QColor(0xa9, 0xc4, 0xe8); }

int countColor(const QImage& img, const QColor& c) {
  int n = 0;
  for (int y = 0; y < img.height(); ++y) {
    for (int x = 0; x < img.width(); ++x) {
      if (img.pixelColor(x, y) == c) ++n;
    }
  }
  return n;
}

client::ChangeFeedSyncer::Timing fast() {
  client::ChangeFeedSyncer::Timing t;
  t.activeMs = 60'000;
  t.inactiveMs = 120'000;
  t.listPollMs = 90'000;
  t.backoffMinMs = 50;
  t.backoffMaxMs = 200;
  return t;
}

QByteArray readAll(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

class TestExportPrint : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QString owner_;
  QString url_;
  QString project_;

  QHash<QString, QString> env() const {
    return {{QStringLiteral("HOME"), dir_->path()},
            {QStringLiteral("ZPROJECT_CONFIG_DIR"), dir_->filePath(QStringLiteral("config"))},
            {QStringLiteral("ZPROJECT_CACHE_DIR"), dir_->filePath(QStringLiteral("cache"))}};
  }
  MainWindow::Options options() {
    MainWindow::Options o;
    o.env = env();
    o.paths = client::desktopPaths(o.env, false);
    o.runner = [](const QString&, const QStringList&, const QByteArray&) { return client::CommandResult{-1, {}, {}}; };
    o.timing = fast();
    o.startupUpdateCheck = false;
    o.today = QDate(2027, 1, 6);
    return o;
  }
  void login() {
    client::ClientConfig c = client::loadClientConfig(env(), QString());
    c.url = url_;
    c.store = QStringLiteral("file");
    QVERIFY(client::saveClientConfig(c).ok());
    client::TokenStore store(c.tokenFile, client::runCommand, false);
    QVERIFY(store.writeFile(owner_).ok());
  }
  std::unique_ptr<MainWindow> open() {
    auto w = std::make_unique<MainWindow>(options());
    w->resize(1100, 600);
    w->show();
    if (!QTest::qWaitForWindowExposed(w.get())) qWarning("not exposed");
    if (!QTest::qWaitFor([&] { return w->session() && w->session()->health() == Health::Online; }, 5000) ||
        !QTest::qWaitFor([&] { return w->projectList()->count() == 1; }, 5000)) {
      qWarning("window did not come online");
    }
    w->selectProject(project_);
    return w;
  }
  // Runs `trigger` and waits for ExportActions::finished.
  static QList<QVariant> run(MainWindow& w, const std::function<void()>& trigger) {
    QSignalSpy spy(w.exportActions(), &ExportActions::finished);
    trigger();
    if (spy.isEmpty() && !spy.wait(5000)) return {};
    return spy.takeFirst();
  }

 private slots:
  void initTestCase() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init() {
    if (!QByteArray(QTest::currentTestFunction()).startsWith("window")) return;
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    store_ = std::move(store::SqliteStore::open(QStringLiteral(":memory:"))).value();
    http_ = std::make_unique<server::http::Server>([this](const server::http::Request& r) { return api_->handle(r); });
    QVERIFY(http_->listen(QHostAddress::LocalHost, 0));
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
    Project p;
    p.name = QStringLiteral("Office move");
    project_ = store_->createProject(p, kOther).value().id;
    auto add = [&](const QString& title, const QString& start, const QString& finish) {
      Task t;
      t.project = project_;
      t.title = title;
      t.mode = QStringLiteral("manual");
      t.start = start;
      t.finish = finish;
      t.duration = 3;
      return store_->createTask(t, kOther).value().id;
    };
    const QString pack = add(QStringLiteral("Pack"), QStringLiteral("2027-01-04"), QStringLiteral("2027-01-06"));
    const QString move = add(QStringLiteral("Move"), QStringLiteral("2027-01-07"), QStringLiteral("2027-01-11"));
    Dependency d;
    d.project = project_;
    d.predecessor = pack;
    d.successor = move;
    QVERIFY(store_->createDependency(d, kOther).ok());
    login();
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  // ---- ProjectPrintout ----

  void paginates() {
    ProjectPrintout out(plan(120), {});
    out.layout(QSizeF(760, 540));  // Letter landscape less margins
    QVERIFY(out.pageCount() > 1);
    QList<int> all;
    for (int p = 0; p < out.pageCount(); ++p) {
      QVERIFY(!out.rowsOnPage(p).isEmpty());
      all += out.rowsOnPage(p);
    }
    QCOMPARE(all.size(), 121);
    for (int i = 0; i < all.size(); ++i) QCOMPARE(all[i], i);  // in order, each once
    // A taller page holds more rows.
    ProjectPrintout tall(plan(120), {});
    tall.layout(QSizeF(540, 760));
    QVERIFY(tall.rowsOnPage(0).size() > out.rowsOnPage(0).size());
  }

  void timescaleFitsTheSpan() {
    ProjectPrintout days(plan(2, 3), {});
    days.layout(QSizeF(760, 540));
    QVERIFY(days.hasGantt());
    QCOMPARE(days.unit(), ProjectPrintout::Unit::Day);
    QVERIFY(days.from() <= QDate(2027, 1, 4));
    ProjectPrintout weeks(plan(12), {});
    weeks.layout(QSizeF(760, 540));
    QCOMPARE(weeks.unit(), ProjectPrintout::Unit::Week);
    QCOMPARE(weeks.from().dayOfWeek(), 1);  // starts on a Monday
    ProjectPrintout years(plan(150), {});
    years.layout(QSizeF(760, 540));
    QVERIFY(years.unit() == ProjectPrintout::Unit::Month || years.unit() == ProjectPrintout::Unit::Quarter);
    QCOMPARE(years.from().day(), 1);
    // Nothing dated, or no room: the table alone.
    ExportData undated;
    undated.tasks.append(task(QStringLiteral("a"), QStringLiteral("A"), 0, {}, {}, {}));
    ProjectPrintout none(undated, {});
    none.layout(QSizeF(760, 540));
    QVERIFY(!none.hasGantt());
    ProjectPrintout narrow(plan(5), {});
    narrow.layout(QSizeF(400, 700));
    QVERIFY(!narrow.hasGantt());
  }

  void paintsBarsOnWhite() {
    PrintSettings s;
    s.subtitle = QStringLiteral("Printed today");
    s.note = QStringLiteral("Offline");
    ProjectPrintout out(plan(6), s);
    out.layout(QSizeF(760, 540));
    QImage img(760, 540, QImage::Format_RGB32);
    img.fill(Qt::black);
    QPainter p(&img);
    out.paintPage(p, 0);
    p.end();
    QCOMPARE(img.pixelColor(2, 539), QColor(Qt::white));  // light whatever the theme
    QVERIFY(countColor(img, barColor()) > 50);          // the task bars
  }

  void pdfHasEveryPage() {
    QBuffer buf;
    QVERIFY(buf.open(QIODevice::WriteOnly));
    int pages = 0;
    {
      QPdfWriter pdf(&buf);
      pdf.setResolution(300);
      pdf.setPageSize(QPageSize(QPageSize::A4));
      pdf.setPageOrientation(QPageLayout::Landscape);
      ProjectPrintout out(plan(100), {});
      QVERIFY(out.print(&pdf));
      pages = out.pageCount();
    }
    const QByteArray b = buf.data();
    QVERIFY(b.startsWith("%PDF"));
    QVERIFY(pages > 1);
    QCOMPARE(b.count("/Type /Page\n") + b.count("/Type /Page\r") + b.count("/Type /Page/") +
                 b.count("/Type /Page>"),
             pages);
  }

  void emptyProjectPrintsOnePage() {
    ExportData d;
    d.project.name = QStringLiteral("Empty");
    ProjectPrintout out(d, {});
    out.layout(QSizeF(760, 540));
    QCOMPARE(out.pageCount(), 1);
    QImage img(760, 540, QImage::Format_RGB32);
    QPainter p(&img);
    out.paintPage(p, 0);
  }

  void kindFromPathOrFilter() {
    const QStringList f = ExportActions::fileFilters();
    QCOMPARE(ExportActions::kindFor(QStringLiteral("a.ODS"), {}), std::optional(Kind::Ods));
    QCOMPARE(ExportActions::kindFor(QStringLiteral("a.xml"), f.at(2)), std::optional(Kind::Mspdi));  // the extension wins
    QCOMPARE(ExportActions::kindFor(QStringLiteral("a"), f.at(3)), std::optional(Kind::Pdf));
    QCOMPARE(ExportActions::kindFor(QStringLiteral("a.txt"), QString()), std::nullopt);
  }

  // ---- the window ----

  void windowExportsEveryKind() {
    auto w = open();
    QVERIFY(w->action(QStringLiteral("actionExport"))->isEnabled());
    QCOMPARE(w->action(QStringLiteral("actionExport"))->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    QCOMPARE(w->action(QStringLiteral("actionPrint"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_P));
    QString suggested;
    QString chosen;
    QString filterIn;
    w->exportActions()->setSavePathProvider([&](const QString& s, const QStringList&, QString* filter) {
      suggested = s;
      *filter = filterIn;
      return chosen;
    });
    const QStringList filters = ExportActions::fileFilters();

    // MS Project XML by default, named after the project; links included.
    chosen = dir_->filePath(QStringLiteral("plan.xml"));
    filterIn = filters.at(0);
    QList<QVariant> r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QCOMPARE(r.value(1).toBool(), true);
    QVERIFY(suggested.endsWith(QLatin1String("/Office-move.xml")));
    const QByteArray xml = readAll(chosen);
    QVERIFY(xml.contains("<Title>Office move</Title>"));
    QVERIFY(xml.contains("<PredecessorLink>"));

    // No extension: the filter's is added. The next suggestion uses the
    // folder and kind used last.
    chosen = dir_->filePath(QStringLiteral("sheet"));
    filterIn = filters.at(1);
    r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QVERIFY(r.value(1).toBool());
    QCOMPARE(r.value(0).toString(), dir_->filePath(QStringLiteral("sheet.ods")));
    QVERIFY(readAll(dir_->filePath(QStringLiteral("sheet.ods"))).startsWith("PK\x03\x04"));

    chosen = dir_->filePath(QStringLiteral("tasks.csv"));
    r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QCOMPARE(suggested, dir_->filePath(QStringLiteral("Office-move.ods")));
    QVERIFY(readAll(chosen).contains(",1,Move,task,manual,3,2027-01-07,2027-01-11,1,"));

    chosen = dir_->filePath(QStringLiteral("all.json"));
    r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QVERIFY(exportDataFromJson(readAll(chosen)).ok());

    chosen = dir_->filePath(QStringLiteral("print.pdf"));
    r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QVERIFY(readAll(chosen).startsWith("%PDF"));

    // Canceling the dialog does nothing.
    chosen.clear();
    QSignalSpy spy(w->exportActions(), &ExportActions::finished);
    w->action(QStringLiteral("actionExport"))->trigger();
    QTest::qWait(100);
    QCOMPARE(spy.count(), 0);
  }

  void windowExportAsPdfAndPrint() {
    auto w = open();
    const QString pdf = dir_->filePath(QStringLiteral("office"));
    w->exportActions()->setSavePathProvider([&](const QString&, const QStringList& filters, QString*) {
      return filters.size() == 1 ? pdf : QString();  // only the PDF filter is offered
    });
    QList<QVariant> r = run(*w, [&] { w->action(QStringLiteral("actionExportPdf"))->trigger(); });
    QVERIFY(r.value(1).toBool());
    QVERIFY(readAll(pdf + QStringLiteral(".pdf")).startsWith("%PDF"));

    // Print… to a PDF printer; the printer was set up landscape.
    const QString printed = dir_->filePath(QStringLiteral("printed.pdf"));
    bool landscape = false;
    w->exportActions()->setPrintDialogProvider([&](QPrinter* p) {
      landscape = p->pageLayout().orientation() == QPageLayout::Landscape;
      p->setOutputFormat(QPrinter::PdfFormat);
      p->setOutputFileName(printed);
      return true;
    });
    r = run(*w, [&] { w->action(QStringLiteral("actionPrint"))->trigger(); });
    QVERIFY(r.value(1).toBool());
    QVERIFY(landscape);
    QVERIFY(readAll(printed).startsWith("%PDF"));
    QVERIFY(w->statusBar()->currentMessage().contains(QStringLiteral("Office move")));

    // Canceled: nothing printed.
    w->exportActions()->setPrintDialogProvider([](QPrinter*) { return false; });
    QSignalSpy spy(w->exportActions(), &ExportActions::finished);
    w->action(QStringLiteral("actionPrint"))->trigger();
    QTest::qWait(100);
    QCOMPARE(spy.count(), 0);
    QVERIFY(!w->exportActions()->busy());
  }

  void windowOfflineExportsFromTheCacheAfterAsking() {
    auto w = open();
    http_.reset();  // the server goes away
    w->session()->refresh();
    QVERIFY(QTest::qWaitFor([&] { return w->session()->health() == Health::Offline; }, 5000));
    QVERIFY(w->action(QStringLiteral("actionExport"))->isEnabled());  // reading works offline
    const QString csv = dir_->filePath(QStringLiteral("offline.csv"));
    w->exportActions()->setSavePathProvider([&](const QString&, const QStringList&, QString*) { return csv; });
    QString asked;
    bool answer = false;
    w->exportActions()->setConfirmProvider([&](const QString&, const QString& text) {
      asked = text;
      return answer;
    });
    QList<QVariant> r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QVERIFY(asked.contains(QStringLiteral("no dependencies")));
    QCOMPARE(r.value(1).toBool(), false);
    QVERIFY(!QFile::exists(csv));
    answer = true;
    r = run(*w, [&] { w->action(QStringLiteral("actionExport"))->trigger(); });
    QVERIFY(r.value(1).toBool());
    const QByteArray b = readAll(csv);
    QVERIFY(b.contains(",Pack,"));
    QVERIFY(b.contains(",Move,task,manual,3,2027-01-07,2027-01-11,,"));  // no predecessors offline

    // Printing offline doesn't ask.
    asked.clear();
    const QString printed = dir_->filePath(QStringLiteral("offline.pdf"));
    w->exportActions()->setPrintDialogProvider([&](QPrinter* p) {
      p->setOutputFormat(QPrinter::PdfFormat);
      p->setOutputFileName(printed);
      return true;
    });
    r = run(*w, [&] { w->action(QStringLiteral("actionPrint"))->trigger(); });
    QVERIFY(r.value(1).toBool());
    QVERIFY(asked.isEmpty());
    QVERIFY(readAll(printed).startsWith("%PDF"));
  }

  void windowNeedsAProject() {
    auto w = open();
    w->projectList()->setCurrentItem(nullptr);
    QVERIFY(!w->action(QStringLiteral("actionExport"))->isEnabled());
    QVERIFY(!w->action(QStringLiteral("actionExportPdf"))->isEnabled());
    QVERIFY(!w->action(QStringLiteral("actionPrint"))->isEnabled());
  }
};

QTEST_MAIN(TestExportPrint)
#include "tst_exportprint.moc"
