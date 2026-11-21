# The zproject desktop app (Qt Widgets). Off with -DZPROJECT_BUILD_GUI=OFF.
set(CMAKE_AUTORCC ON)
add_executable(zproject
  main.cpp
  MainWindow.cpp MainWindow.hpp
  resources.qrc)
target_link_libraries(zproject PRIVATE zproject_core Qt6::Widgets)
zproject_set_warnings(zproject)
# macOS: zproject.app at the top of the .dmg (cmake/MacBundle.cmake).
install(TARGETS zproject
  RUNTIME DESTINATION bin COMPONENT desktop
  BUNDLE DESTINATION . COMPONENT desktop)
