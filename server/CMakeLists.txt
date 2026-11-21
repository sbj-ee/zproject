# zproject_server_lib (HTTP layer, auth, rate limits, audit, /v1, /mcp,
# alerts) and the zprojectd executable. Qt Core + Network only, no GUI
# (SPEC.md §4.3).

# Embed api/openapi.json (generated from api/openapi.yaml) for
# GET /v1/openapi.json and the MCP resource zproject://openapi.
set(_zp_openapi "${PROJECT_SOURCE_DIR}/api/openapi.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_zp_openapi}")
file(READ "${_zp_openapi}" ZP_OPENAPI_JSON)
string(FIND "${ZP_OPENAPI_JSON}" ")zpjson\"" _zp_bad)
if(NOT _zp_bad EQUAL -1)
  message(FATAL_ERROR "api/openapi.json contains the raw-string delimiter")
endif()
# Split into raw-string pieces well under the 65,536-character literal limit.
string(LENGTH "${ZP_OPENAPI_JSON}" _zp_len)
set(_zp_pieces "")
set(_zp_pos 0)
while(_zp_pos LESS _zp_len)
  string(SUBSTRING "${ZP_OPENAPI_JSON}" ${_zp_pos} 16000 _zp_piece)
  string(APPEND _zp_pieces "    R\"zpjson(${_zp_piece})zpjson\",\n")
  math(EXPR _zp_pos "${_zp_pos} + 16000")
endwhile()
set(ZP_OPENAPI_JSON "${_zp_pieces}")
configure_file(OpenApiData.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/OpenApiData.cpp" @ONLY)

add_library(zproject_server_lib STATIC
  ServerConfig.cpp ServerConfig.hpp
  Auth.cpp Auth.hpp
  RateLimiter.cpp RateLimiter.hpp
  Notifier.cpp Notifier.hpp
  Api.cpp Api.hpp
  Mcp.cpp Mcp.hpp
  http/HttpTypes.hpp
  http/HttpParser.cpp http/HttpParser.hpp
  http/HttpServer.cpp http/HttpServer.hpp
  "${CMAKE_CURRENT_BINARY_DIR}/OpenApiData.cpp")
target_link_libraries(zproject_server_lib PUBLIC zproject_core zproject_store Qt6::Network)
zproject_set_warnings(zproject_server_lib)

add_executable(zprojectd main.cpp)
target_link_libraries(zprojectd PRIVATE zproject_server_lib zproject_store)
zproject_set_warnings(zprojectd)
# The server is packaged for Linux only (zprojectd_<ver>_amd64.deb).
if(NOT APPLE)
  install(TARGETS zprojectd RUNTIME DESTINATION bin COMPONENT server)
endif()
