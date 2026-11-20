# Vendored SQLite amalgamation (README.zproject.md). Third-party code: no
# zproject warning flags, and its header is a SYSTEM include so it never trips
# -Werror in zproject's own targets.
add_library(zproject_sqlite STATIC sqlite3.c)
target_include_directories(zproject_sqlite SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(zproject_sqlite PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(zproject_sqlite PUBLIC
  SQLITE_THREADSAFE=1          # serialized; zprojectd uses one writer + readers
  SQLITE_DQS=0                 # no double-quoted string literals
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_OMIT_LOAD_EXTENSION   # no run-time extensions
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_USE_ALLOCA
  SQLITE_LIKE_DOESNT_MATCH_BLOBS)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zproject_sqlite PRIVATE -w)
endif()
find_package(Threads REQUIRED)
target_link_libraries(zproject_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
