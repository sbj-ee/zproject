# Fetch the SQLite amalgamation from sqlite.org at configure time and verify it
# against the hashes pinned below; any mismatch is a fatal error (SPEC.md §4.3).
#
# Where the sources come from, in order:
#   1. SQLITE_SOURCE_DIR: a directory holding sqlite3.c and sqlite3.h, already
#      extracted (e.g. for a packaging build). Each file is checked against its
#      pinned SHA3-256.
#   2. <ZPROJECT_SQLITE_CACHE>/<zip>: a zip downloaded earlier. Its SHA3-256 and
#      SHA-256 are checked every time; a cached zip that does not match is an
#      error (delete it to re-download), never silently replaced.
#   3. A download from sqlite.org into the cache. The zip is verified before it
#      is renamed into place, so the cache only ever holds a verified file.
#      ZPROJECT_SQLITE_OFFLINE=ON (or FETCHCONTENT_FULLY_DISCONNECTED=ON) turns
#      the download off; a build then needs 1 or 2.
#
# The cache defaults to $ZPROJECT_SQLITE_CACHE, else $XDG_CACHE_HOME/zproject,
# else ~/.cache/zproject. CI caches that directory with actions/cache.
#
# Also runs as a script (tests/sqlite/HashMismatch.cmake uses it):
#   cmake -DZPROJECT_SQLITE_CACHE=<dir> -DOUT_DIR=<dir> [-DZPROJECT_SQLITE_OFFLINE=ON] \
#         [-DSQLITE_SOURCE_DIR=<dir>] -P cmake/FetchSQLite.cmake
#
# To update SQLite: change the version, year and the four hashes below. Take
# the zip's SHA3-256 from https://www.sqlite.org/download.html and check that
# a fresh download matches it; the per-file hashes are SHA3-256 of sqlite3.c
# and sqlite3.h inside that zip. Then bump the version and add a CHANGELOG
# entry.

set(ZPROJECT_SQLITE_VERSION "3.54.1")
set(_zp_sqlite_zip_name "sqlite-amalgamation-3540100")
set(_zp_sqlite_url "https://www.sqlite.org/2026/${_zp_sqlite_zip_name}.zip")
set(_zp_sqlite_zip_sha3_256 "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30")
set(_zp_sqlite_zip_sha256 "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9")
set(_zp_sqlite_c_sha3_256 "e2b869d3003b33c942b7dd7063604fbc53553e1b66609570df39b9b7b558ee81")
set(_zp_sqlite_h_sha3_256 "15ef3f5dbc4a7a980f828fa0d1e473cee5a124e7bb77e9bc70fc0bfa3f9a2af8")

# Fails unless <file>'s <algo> hash is <expected>.
function(_zp_sqlite_check_hash file algo expected)
  file(${algo} "${file}" _actual)
  if(NOT _actual STREQUAL expected)
    string(REPLACE "_" "-" _name "${algo}")
    message(FATAL_ERROR
      "SQLite ${_name} mismatch for ${file}\n"
      "  expected ${expected}\n"
      "  actual   ${_actual}\n"
      "Refusing to build with it. If this is a cached download, delete it and "
      "configure again.")
  endif()
endfunction()

# True in <out> if <dir> holds sqlite3.c and sqlite3.h matching the pins.
function(_zp_sqlite_files_ok dir out)
  set(${out} FALSE PARENT_SCOPE)
  if(NOT EXISTS "${dir}/sqlite3.c" OR NOT EXISTS "${dir}/sqlite3.h")
    return()
  endif()
  file(SHA3_256 "${dir}/sqlite3.c" _c)
  file(SHA3_256 "${dir}/sqlite3.h" _h)
  if(_c STREQUAL _zp_sqlite_c_sha3_256 AND _h STREQUAL _zp_sqlite_h_sha3_256)
    set(${out} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(_zp_sqlite_default_cache out)
  if(DEFINED ENV{ZPROJECT_SQLITE_CACHE} AND NOT "$ENV{ZPROJECT_SQLITE_CACHE}" STREQUAL "")
    set(_dir "$ENV{ZPROJECT_SQLITE_CACHE}")
  elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_dir "$ENV{XDG_CACHE_HOME}/zproject")
  elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(_dir "$ENV{HOME}/.cache/zproject")
  else()
    set(_dir "${CMAKE_BINARY_DIR}/sqlite-cache")
  endif()
  set(${out} "${_dir}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to a directory holding verified sqlite3.c and sqlite3.h,
# extracting into <out_dir> when needed.
function(zproject_fetch_sqlite out_dir out_var)
  # 1. Pre-extracted sources.
  if(SQLITE_SOURCE_DIR)
    foreach(_f sqlite3.c sqlite3.h)
      if(NOT EXISTS "${SQLITE_SOURCE_DIR}/${_f}")
        message(FATAL_ERROR "SQLITE_SOURCE_DIR=${SQLITE_SOURCE_DIR} has no ${_f}")
      endif()
    endforeach()
    _zp_sqlite_check_hash("${SQLITE_SOURCE_DIR}/sqlite3.c" SHA3_256 "${_zp_sqlite_c_sha3_256}")
    _zp_sqlite_check_hash("${SQLITE_SOURCE_DIR}/sqlite3.h" SHA3_256 "${_zp_sqlite_h_sha3_256}")
    message(STATUS "SQLite ${ZPROJECT_SQLITE_VERSION}: ${SQLITE_SOURCE_DIR} (SHA3-256 verified)")
    set(${out_var} "${SQLITE_SOURCE_DIR}" PARENT_SCOPE)
    return()
  endif()

  # Already extracted into this build tree and still matching: reuse it, so a
  # re-configure does not touch sqlite3.c and rebuild it.
  _zp_sqlite_files_ok("${out_dir}" _ok)
  if(_ok)
    message(STATUS "SQLite ${ZPROJECT_SQLITE_VERSION}: ${out_dir} (SHA3-256 verified)")
    set(${out_var} "${out_dir}" PARENT_SCOPE)
    return()
  endif()

  set(_cache "${ZPROJECT_SQLITE_CACHE}")
  if(NOT _cache)
    _zp_sqlite_default_cache(_cache)
  endif()
  file(MAKE_DIRECTORY "${_cache}")
  set(_zip "${_cache}/${_zp_sqlite_zip_name}.zip")
  # Parallel configures share the cache.
  file(LOCK "${_cache}/${_zp_sqlite_zip_name}.lock" GUARD FUNCTION TIMEOUT 600)

  if(NOT EXISTS "${_zip}")
    # 2./3. Not cached yet: download, verify, then move into the cache.
    if(ZPROJECT_SQLITE_OFFLINE OR FETCHCONTENT_FULLY_DISCONNECTED)
      message(FATAL_ERROR
        "SQLite: ${_zip} is not cached and downloads are off "
        "(ZPROJECT_SQLITE_OFFLINE / FETCHCONTENT_FULLY_DISCONNECTED). Put "
        "${_zp_sqlite_zip_name}.zip from ${_zp_sqlite_url} in ${_cache}, or set "
        "SQLITE_SOURCE_DIR.")
    endif()
    message(STATUS "SQLite ${ZPROJECT_SQLITE_VERSION}: downloading ${_zp_sqlite_url}")
    set(_part "${_zip}.part")
    file(REMOVE "${_part}")
    file(DOWNLOAD "${_zp_sqlite_url}" "${_part}"
      TLS_VERIFY ON INACTIVITY_TIMEOUT 60 TIMEOUT 600 STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_part}")
      message(FATAL_ERROR "SQLite: download of ${_zp_sqlite_url} failed: ${_status}")
    endif()
    _zp_sqlite_check_hash("${_part}" SHA3_256 "${_zp_sqlite_zip_sha3_256}")
    _zp_sqlite_check_hash("${_part}" SHA256 "${_zp_sqlite_zip_sha256}")
    file(RENAME "${_part}" "${_zip}")
  endif()

  _zp_sqlite_check_hash("${_zip}" SHA3_256 "${_zp_sqlite_zip_sha3_256}")
  _zp_sqlite_check_hash("${_zip}" SHA256 "${_zp_sqlite_zip_sha256}")

  set(_tmp "${out_dir}.extract")
  file(REMOVE_RECURSE "${_tmp}")
  file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_tmp}"
    PATTERNS "${_zp_sqlite_zip_name}/sqlite3.c" "${_zp_sqlite_zip_name}/sqlite3.h")
  _zp_sqlite_check_hash("${_tmp}/${_zp_sqlite_zip_name}/sqlite3.c" SHA3_256 "${_zp_sqlite_c_sha3_256}")
  _zp_sqlite_check_hash("${_tmp}/${_zp_sqlite_zip_name}/sqlite3.h" SHA3_256 "${_zp_sqlite_h_sha3_256}")
  file(REMOVE_RECURSE "${out_dir}")
  file(RENAME "${_tmp}/${_zp_sqlite_zip_name}" "${out_dir}")
  file(REMOVE_RECURSE "${_tmp}")
  message(STATUS "SQLite ${ZPROJECT_SQLITE_VERSION}: ${_zip} (SHA3-256 + SHA-256 verified)")
  set(${out_var} "${out_dir}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT OUT_DIR)
    message(FATAL_ERROR "usage: cmake -DOUT_DIR=<dir> [-DZPROJECT_SQLITE_CACHE=<dir>] -P FetchSQLite.cmake")
  endif()
  zproject_fetch_sqlite("${OUT_DIR}" _dir)
endif()
