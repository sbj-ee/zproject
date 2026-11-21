add_executable(zproject-cli main.cpp)
target_link_libraries(zproject-cli PRIVATE zproject_client)
zproject_set_warnings(zproject-cli)
if(APPLE AND ZPROJECT_BUILD_GUI)
  # Inside zproject.app, deployed with it (cmake/MacDeploy.cmake.in).
  install(TARGETS zproject-cli RUNTIME DESTINATION zproject.app/Contents/MacOS COMPONENT desktop)
else()
  install(TARGETS zproject-cli RUNTIME DESTINATION bin COMPONENT desktop)
endif()
