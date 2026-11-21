# SQLite amalgamation, statically linked (SPEC.md §4.3): fetched from sqlite.org
# and hash-verified by FetchSQLite.cmake, not kept in the repository.
# Third-party code: no zproject warning flags, and its header is a SYSTEM
# include so it never trips -Werror in zproject's own targets.
set(ZPROJECT_SQLITE_CACHE "" CACHE PATH
  "Download cache for the SQLite zip (default: $ZPROJECT_SQLITE_CACHE, $XDG_CACHE_HOME/zproject or ~/.cache/zproject)")
set(SQLITE_SOURCE_DIR "" CACHE PATH
  "Directory with an already extracted sqlite3.c/sqlite3.h (still hash-checked); skips the download")
option(ZPROJECT_SQLITE_OFFLINE "Never download SQLite; use the cache or SQLITE_SOURCE_DIR" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/FetchSQLite.cmake")
zproject_fetch_sqlite("${CMAKE_BINARY_DIR}/_deps/sqlite-${ZPROJECT_SQLITE_VERSION}" ZPROJECT_SQLITE_DIR)

add_library(zproject_sqlite STATIC "${ZPROJECT_SQLITE_DIR}/sqlite3.c")
target_include_directories(zproject_sqlite SYSTEM PUBLIC "${ZPROJECT_SQLITE_DIR}")
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
