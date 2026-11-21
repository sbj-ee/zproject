# The zproject desktop app (Qt Widgets). Off with -DZPROJECT_BUILD_GUI=OFF.
set(CMAKE_AUTORCC ON)
add_executable(zproject
  main.cpp
  MainWindow.cpp MainWindow.hpp
  resources.qrc)
target_link_libraries(zproject PRIVATE zproject_core Qt6::Widgets)
zproject_set_warnings(zproject)
# Linux: /usr/bin/zproject. macOS: the zproject.app bundle is installed by
# cmake/Packaging.cmake, after cmake/MacBundle.cmake has made it a bundle.
if(NOT APPLE)
  install(TARGETS zproject RUNTIME DESTINATION bin COMPONENT desktop)
endif()
