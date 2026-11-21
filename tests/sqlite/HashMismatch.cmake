# ctest sqlite_hash_verification: FetchSQLite.cmake must refuse sources whose
# hash does not match the pins, and accept ones that do. Runs FetchSQLite.cmake
# in script mode, offline, against a scratch cache.
#   -DFETCH=<cmake/FetchSQLite.cmake> -DGOOD_DIR=<verified sqlite dir> -DWORK=<scratch dir>
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/cache" "${WORK}/bad-src")

# Runs FetchSQLite.cmake with <args...>; sets _rc and _log.
macro(_run)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DOUT_DIR=${WORK}/out
      -DZPROJECT_SQLITE_CACHE=${WORK}/cache -DZPROJECT_SQLITE_OFFLINE=ON ${ARGN}
      -P "${FETCH}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _log ERROR_VARIABLE _log)
endmacro()

macro(_expect_fail what pattern)
  if(_rc EQUAL 0)
    message(FATAL_ERROR "${what}: accepted, expected a failure\n${_log}")
  endif()
  # CMake wraps long messages, so a long build path can split the phrase.
  string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_log}")
  if(NOT _flat MATCHES "${pattern}")
    message(FATAL_ERROR "${what}: failed, but without \"${pattern}\"\n${_log}")
  endif()
  if(EXISTS "${WORK}/out/sqlite3.c")
    message(FATAL_ERROR "${what}: sources were extracted despite the failure")
  endif()
  message(STATUS "ok: ${what} rejected")
endmacro()

# 1. A cached zip with the wrong hash.
file(WRITE "${WORK}/cache/sqlite-amalgamation-3540100.zip" "not the sqlite.org zip\n")
_run()
_expect_fail("cached zip with a wrong hash" "SQLite SHA3-256 mismatch")
file(REMOVE "${WORK}/cache/sqlite-amalgamation-3540100.zip")

# 2. Nothing cached and downloads off.
_run()
_expect_fail("offline with an empty cache" "is not cached and downloads are off")

# 3. SQLITE_SOURCE_DIR with one byte appended to sqlite3.c.
file(COPY "${GOOD_DIR}/sqlite3.c" "${GOOD_DIR}/sqlite3.h" DESTINATION "${WORK}/bad-src")
file(APPEND "${WORK}/bad-src/sqlite3.c" " ")
_run(-DSQLITE_SOURCE_DIR=${WORK}/bad-src)
_expect_fail("tampered SQLITE_SOURCE_DIR" "SQLite SHA3-256 mismatch")

# 4. The verified sources are accepted.
_run(-DSQLITE_SOURCE_DIR=${GOOD_DIR})
if(NOT _rc EQUAL 0 OR NOT _log MATCHES "SHA3-256 verified")
  message(FATAL_ERROR "verified sources rejected\n${_log}")
endif()
message(STATUS "ok: verified sources accepted")
file(REMOVE_RECURSE "${WORK}")
