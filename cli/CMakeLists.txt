add_executable(zproject-cli main.cpp)
target_link_libraries(zproject-cli PRIVATE zproject_client)
zproject_set_warnings(zproject-cli)
# macOS with the GUI: installed inside zproject.app by cmake/Packaging.cmake,
# so it lands after the bundle and before macdeployqt runs.
if(NOT (APPLE AND ZPROJECT_BUILD_GUI))
  install(TARGETS zproject-cli RUNTIME DESTINATION bin COMPONENT desktop)
endif()
