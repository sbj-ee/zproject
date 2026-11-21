add_executable(zproject-cli main.cpp)
target_link_libraries(zproject-cli PRIVATE zproject_core)
zproject_set_warnings(zproject-cli)
install(TARGETS zproject-cli RUNTIME DESTINATION bin COMPONENT desktop)
