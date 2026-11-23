# The zproject desktop app (Qt Widgets). Off with -DZPROJECT_BUILD_GUI=OFF.
# zproject_app_lib holds the shell so tests/tst_appshell can drive it
# offscreen; the executable is main.cpp.
set(CMAKE_AUTORCC ON)
add_library(zproject_app_lib STATIC
  AppSession.cpp AppSession.hpp
  Dialogs.cpp Dialogs.hpp
  EditDialogs.cpp EditDialogs.hpp
  GanttView.cpp GanttView.hpp
  HelpDialog.cpp HelpDialog.hpp
  Icons.cpp Icons.hpp
  MainWindow.cpp MainWindow.hpp
  OutlineModel.cpp OutlineModel.hpp
  OutlineView.cpp OutlineView.hpp
  TaskInspector.cpp TaskInspector.hpp
  ProjectActions.cpp ProjectActions.hpp
  Shortcuts.cpp Shortcuts.hpp
  SignInDialog.cpp SignInDialog.hpp
  SingleInstance.cpp SingleInstance.hpp
  TaskActions.cpp TaskActions.hpp
  Theme.cpp Theme.hpp
  Minisign.cpp Minisign.hpp
  Update.cpp Update.hpp
  UpdateChecker.cpp UpdateChecker.hpp
  UpdateDialog.cpp UpdateDialog.hpp
  UpdateManager.cpp UpdateManager.hpp
  zproject_app.qrc)
target_link_libraries(zproject_app_lib PUBLIC zproject_client Qt6::Widgets PRIVATE PkgConfig::SODIUM)
zproject_set_warnings(zproject_app_lib)

add_executable(zproject main.cpp)
target_link_libraries(zproject PRIVATE zproject_app_lib)
zproject_set_warnings(zproject)
# Linux: /usr/bin/zproject. macOS: the zproject.app bundle is installed by
# cmake/Packaging.cmake, after cmake/MacBundle.cmake has made it a bundle.
if(NOT APPLE)
  install(TARGETS zproject RUNTIME DESTINATION bin COMPONENT desktop)
endif()
