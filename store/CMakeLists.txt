# zproject_store: the Store interface and its SQLite implementation, schema
# migrations and (later) backup (SPEC.md §4.1, §16).

# Embed store/migrations/NNNN_*.sql into the library as an ordered table, so
# zprojectd needs no data files. Re-runs when a migration is added or edited.
file(GLOB _zp_migrations CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/migrations/[0-9][0-9][0-9][0-9]_*.sql")
list(SORT _zp_migrations)
set(_zp_entries "")
set(_zp_expected 1)
foreach(_file IN LISTS _zp_migrations)
  get_filename_component(_name "${_file}" NAME)
  string(SUBSTRING "${_name}" 0 4 _num)
  math(EXPR _num "${_num}")
  if(NOT _num EQUAL _zp_expected)
    message(FATAL_ERROR "store/migrations: expected migration ${_zp_expected}, found ${_name}")
  endif()
  math(EXPR _zp_expected "${_zp_expected} + 1")
  file(READ "${_file}" _sql)
  string(FIND "${_sql}" ")zpsql\"" _bad)
  if(NOT _bad EQUAL -1)
    message(FATAL_ERROR "${_name} contains the raw-string delimiter")
  endif()
  string(APPEND _zp_entries "    {${_num}, \"${_name}\", R\"zpsql(${_sql})zpsql\"},\n")
endforeach()
configure_file(MigrationsData.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/MigrationsData.cpp" @ONLY)

add_library(zproject_store STATIC
  Database.cpp Database.hpp
  Migrations.cpp Migrations.hpp
  "${CMAKE_CURRENT_BINARY_DIR}/MigrationsData.cpp"
  Store.hpp
  SqliteStore.cpp SqliteStore.hpp)
target_link_libraries(zproject_store PUBLIC zproject_core zproject_sqlite)
zproject_set_warnings(zproject_store)
