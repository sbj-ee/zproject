# zproject_server_lib (router, auth, audit, MCP, alerts, ... as they arrive)
# and the zprojectd executable. Qt Core + Network only, no GUI (SPEC.md §4.3).
add_library(zproject_server_lib STATIC
  ServerConfig.cpp ServerConfig.hpp)
target_link_libraries(zproject_server_lib PUBLIC zproject_core Qt6::Network)
zproject_set_warnings(zproject_server_lib)

add_executable(zprojectd main.cpp)
target_link_libraries(zprojectd PRIVATE zproject_server_lib zproject_store)
zproject_set_warnings(zprojectd)
install(TARGETS zprojectd RUNTIME DESTINATION bin COMPONENT server)
