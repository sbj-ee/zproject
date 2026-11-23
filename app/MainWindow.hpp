#pragma once

#include <QHash>
#include <QDate>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

#include "app/AppSession.hpp"
#include "app/Dialogs.hpp"
#include "client/ChangeFeedSyncer.hpp"
#include "client/DesktopSettings.hpp"
#include "client/TokenStore.hpp"

class QAction;
class QActionGroup;
class QDockWidget;
class QFrame;
class QLabel;
class QListWidget;
class QMessageBox;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QToolBar;
class QToolButton;

namespace zproject::app {

class HelpDialog;
class ActivityView;
class GanttView;
class MilestonesView;
class OutlineView;
class ProjectActions;
class TaskActions;
class TaskInspector;
struct ThemeColors;
class UpdateDialog;
class UpdateManager;
class SignInDialog;

// The desktop shell (SPEC.md §8.1): menus, the project sidebar, a central
// area for the views later versions add, the inspector, the offline banner
// and the status bar. It owns the AppSession (client core from 0.6.0) and
// rebuilds it when the server, token or cache setting changes.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  struct Options {
    QHash<QString, QString> env;           // the process environment
    client::DesktopPaths paths;            // zproject.ini and the cache dir
    client::CommandRunner runner;          // for Keychain/secret-tool; tests fake it
    bool macos = false;
    client::ChangeFeedSyncer::Timing timing;
    bool autoConnect = true;               // false: tests call connectToServer()
    QPalette systemPalette;                // the platform palette before ours
    bool startupUpdateCheck = true;        // false: tests start the updater themselves
    QDate today;                           // the Gantt's today line; invalid: the real date
  };
  static Options defaultOptions();

  explicit MainWindow(Options options, QWidget* parent = nullptr);
  ~MainWindow() override;
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  // Builds the session from zproject.ini, cli.ini and the token store.
  // False when there is no URL or token yet: needsSignIn() is then true.
  bool connectToServer();
  bool needsSignIn() const;
  // First run or a refused token: the sign-in dialog, window-modal.
  SignInDialog* showSignIn(const QString& reason = {});

  // Appearance.
  QString themeChoice() const { return settings_.theme; }
  QString resolvedTheme() const { return resolvedTheme_; }
  void setTheme(const QString& choice);
  int zoomPercent() const { return settings_.zoomPercent; }
  void setZoomPercent(int percent);
  void zoomIn();
  void zoomOut();
  void zoomReset();

  bool editingAllowed() const;
  bool clearCache(QString* message);
  void setCacheEnabled(bool enabled);

  // Dialogs (modeless; one of each).
  HelpDialog* showHelp();
  AboutDialog* showAbout();
  ShortcutsDialog* showShortcuts();
  PreferencesDialog* showPreferences();
  // Help → Check for Updates (SPEC.md §21).
  UpdateDialog* checkForUpdates();
  UpdateManager* updater() const { return updater_; }
  // First run: "Check for updates once a day?" (default yes), then the
  // daily check if it is due. Returns the question box, or nullptr.
  QMessageBox* startUpdates();
  void setUpdateCheckDaily(bool daily);
  QString updateStatePath() const;
  ConnectionInfo connectionInfo() const;

  // Tests: https links go here instead of the desktop browser.
  void setOpenUrlHandler(std::function<void(const QUrl&)> h) { openUrl_ = std::move(h); }

  AppSession* session() const { return session_.get(); }
  const client::DesktopSettings& settings() const { return settings_; }
  QFrame* banner() const { return banner_; }
  QLabel* bannerText() const { return bannerText_; }
  QLabel* connectionStatus() const { return status_; }
  QLabel* refreshStatus() const { return refreshed_; }
  QListWidget* projectList() const { return projects_; }
  // Projects (SPEC.md §7.10, §8.1): the all-projects table (View → All
  // Projects), the details page and the editing actions.
  QTableWidget* projectTable() const { return projectTable_; }
  QToolButton* showClosedButton() const { return showClosed_; }
  QToolBar* toolBar() const { return toolBar_; }
  void setToolbarShown(bool on);
  QStackedWidget* mainStack() const { return stack_; }
  QLabel* projectDetails() const { return projectDetails_; }
  ProjectActions* projectActions() const { return projectActions_; }
  QString currentProjectId() const;
  void selectProject(const QString& id);
  void showProjectTable();
  void showProjectDetails();
  // The WBS outline of the selected project (View → Outline; SPEC.md §7.1).
  OutlineView* outline() const { return outline_; }
  GanttView* gantt() const { return gantt_; }
  // View → Milestones (plan §3.5, SPEC.md §7.1).
  MilestonesView* milestones() const { return milestones_; }
  // View → Activity (plan §3.6, SPEC.md §8.3).
  ActivityView* activity() const { return activity_; }
  TaskActions* taskActions() const { return taskActions_; }
  TaskInspector* taskInspector() const { return taskInspector_; }
  // The "Save your changes?" question while one is open (tests).
  QMessageBox* unsavedBox() const { return unsavedBox_.data(); }
  void showOutline();
  void showGantt();
  void showMilestones();
  void showActivity();
  // The window title for a project (empty: none selected).
  static QString titleFor(const QString& projectName);
  QString viewTaskId() const;
  QAction* action(const QString& objectName) const;

 signals:
  void editingAllowedChanged(bool allowed);

 protected:
  void changeEvent(QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildMenus();
  void buildToolBar();
  void updateWindowTitle(const QString& projectName);
  void buildCentral();
  void buildDocks();
  void buildStatusBar();
  void applyAppearance();
  void applyIcons(const ThemeColors& c);
  void saveSettings();
  void startSession(client::Connection connection);
  void updateConnectionUi();
  void reloadProjects();
  void showProject(const QString& id);
  void fillProjectTable(const QList<QJsonObject>& projects);
  void updateEditActions();
  void showTaskInspector(const QString& taskId);
  // The inspector shows the project (or nothing) instead of a task; asks
  // first when the task editor has unsaved changes.
  void showInspectorText(const QString& html);
  // "Save your changes to X?" Save or Discard, then `then` (not after a
  // Save that fails its checks: the task stays selected).
  void askUnsaved(std::function<void()> then);
  void openUrl(const QUrl& url);
  client::ClientConfig cliConfig() const;
  client::TokenStore tokenStore(const client::ClientConfig& cli) const;
  QString cachePath() const;

  Options options_;
  client::DesktopSettings settings_;
  QString resolvedTheme_;
  std::unique_ptr<AppSession> session_;
  bool signInNeeded_ = false;
  bool lastEditing_ = false;
  std::function<void(const QUrl&)> openUrl_;

  QHash<QString, QAction*> actions_;
  QActionGroup* themeGroup_ = nullptr;
  QFrame* banner_ = nullptr;
  QLabel* bannerText_ = nullptr;
  QPushButton* retry_ = nullptr;
  QPushButton* signInButton_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QLabel* empty_ = nullptr;
  QLabel* projectTitle_ = nullptr;
  QLabel* projectSummary_ = nullptr;
  QListWidget* projects_ = nullptr;
  QToolButton* showClosed_ = nullptr;
  QToolBar* toolBar_ = nullptr;
  QTableWidget* projectTable_ = nullptr;
  QLabel* projectDetails_ = nullptr;
  QPushButton* editButton_ = nullptr;
  QPushButton* deleteButton_ = nullptr;
  ProjectActions* projectActions_ = nullptr;
  // What a selected project shows: its details or its outline.
  enum class ProjectPage { Details, Outline, Gantt, Milestones, Activity };
  ProjectPage page_ = ProjectPage::Details;
  QWidget* detailsPage_ = nullptr;
  QPushButton* outlineButton_ = nullptr;
  OutlineView* outline_ = nullptr;
  GanttView* gantt_ = nullptr;
  MilestonesView* milestones_ = nullptr;
  ActivityView* activity_ = nullptr;
  TaskActions* taskActions_ = nullptr;
  QString pendingSelect_;  // a project just created: select it once the feed has it
  QDockWidget* sidebarDock_ = nullptr;
  QDockWidget* inspectorDock_ = nullptr;
  QLabel* inspector_ = nullptr;
  QStackedWidget* inspectorStack_ = nullptr;
  TaskInspector* taskInspector_ = nullptr;
  QPointer<QMessageBox> unsavedBox_;
  std::function<void()> afterUnsaved_;
  bool closeAnyway_ = false;
  QLabel* status_ = nullptr;
  QLabel* refreshed_ = nullptr;
  QLabel* readOnly_ = nullptr;

  QPointer<HelpDialog> help_;
  QPointer<AboutDialog> about_;
  QPointer<ShortcutsDialog> shortcuts_;
  QPointer<PreferencesDialog> preferences_;
  QPointer<SignInDialog> signIn_;
  UpdateManager* updater_ = nullptr;
};

}  // namespace zproject::app
