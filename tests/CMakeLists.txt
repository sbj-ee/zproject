# Qt Test suites. Each tst_<name>.cpp is one executable and one ctest test.
function(zproject_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN})
  zproject_set_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

zproject_add_test(tst_core zproject_core)
zproject_add_test(tst_serverconfig zproject_server_lib)
zproject_add_test(tst_model zproject_core)
zproject_add_test(tst_rollup zproject_core)
zproject_add_test(tst_migrations zproject_store)
zproject_add_test(tst_store zproject_store)
zproject_add_test(tst_audit zproject_store)
zproject_add_test(tst_backup zproject_store)
zproject_add_test(tst_httpparser zproject_server_lib)
zproject_add_test(tst_httpserver zproject_server_lib)
zproject_add_test(tst_auth zproject_server_lib)
zproject_add_test(tst_api zproject_server_lib)
zproject_add_test(tst_cli zproject_client zproject_server_lib)

# Desktop client core (SPEC.md §16.5): the fault-injecting mock server is a
# test-only library.
add_library(zproject_test_mock STATIC MockServer.cpp MockServer.hpp)
target_link_libraries(zproject_test_mock PUBLIC Qt6::Network)
zproject_set_warnings(zproject_test_mock)
zproject_add_test(tst_clientcore zproject_client)
zproject_add_test(tst_cache zproject_client)
zproject_add_test(tst_editflow zproject_client)
zproject_add_test(tst_asyncapi zproject_client zproject_test_mock)
zproject_add_test(tst_syncer zproject_client zproject_test_mock zproject_server_lib)

# SQLite hash pinning (cmake/FetchSQLite.cmake): wrong hashes are refused.
add_test(NAME sqlite_hash_verification COMMAND ${CMAKE_COMMAND}
  -DFETCH=${PROJECT_SOURCE_DIR}/cmake/FetchSQLite.cmake
  -DGOOD_DIR=${ZPROJECT_SQLITE_DIR}
  -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sqlite-hash-test
  -P ${CMAKE_CURRENT_SOURCE_DIR}/sqlite/HashMismatch.cmake)

# Every binary reports the single project() version.
add_test(NAME zprojectd_version COMMAND zprojectd --version)
set_tests_properties(zprojectd_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^zprojectd ${PROJECT_VERSION}\napiVersion 1\nschemaVersion [0-9]+\n$")
add_test(NAME zprojectd_refuses_non_loopback COMMAND zprojectd --listen 0.0.0.0:8788)
set_tests_properties(zprojectd_refuses_non_loopback PROPERTIES
  PASS_REGULAR_EXPRESSION "only loopback addresses are allowed")
# --migrate-only creates a fresh database at the head schema, and is a no-op
# the second time.
set(_zp_mdb "${CMAKE_CURRENT_BINARY_DIR}/migrate-only-test.db")
add_test(NAME zprojectd_migrate_clean
  COMMAND ${CMAKE_COMMAND} -E rm -f "${_zp_mdb}" "${_zp_mdb}-wal" "${_zp_mdb}-shm")
add_test(NAME zprojectd_migrate_only COMMAND zprojectd --db "${_zp_mdb}" --migrate-only)
add_test(NAME zprojectd_migrate_again COMMAND zprojectd --db "${_zp_mdb}" --migrate-only)
set_tests_properties(zprojectd_migrate_clean PROPERTIES FIXTURES_SETUP zp_mdb)
set_tests_properties(zprojectd_migrate_only PROPERTIES FIXTURES_REQUIRED zp_mdb
  PASS_REGULAR_EXPRESSION "is at schema [1-9][0-9]*")
set_tests_properties(zprojectd_migrate_again PROPERTIES FIXTURES_REQUIRED zp_mdb DEPENDS zprojectd_migrate_only
  PASS_REGULAR_EXPRESSION "is at schema [1-9][0-9]*")
add_test(NAME zproject_cli_version COMMAND zproject-cli --version)
set_tests_properties(zproject_cli_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^zproject-cli ${PROJECT_VERSION}\n$")
if(ZPROJECT_BUILD_GUI)
  add_test(NAME zproject_version COMMAND zproject --version)
  set_tests_properties(zproject_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^zproject ${PROJECT_VERSION}\n$")
endif()

# The desktop app shell (SPEC.md §8), offscreen.
if(ZPROJECT_BUILD_GUI)
  zproject_add_test(tst_appshell zproject_app_lib zproject_test_mock)
  # Projects: list, details, editor, dry-run preview, the 412 conflict
  # dialog, against the real server on loopback.
  zproject_add_test(tst_projects zproject_app_lib zproject_server_lib)
  zproject_add_test(tst_outline zproject_app_lib zproject_server_lib)
  # The signed in-app updater (SPEC.md §21) against a mock release feed.
  zproject_add_test(tst_update zproject_app_lib zproject_test_mock PkgConfig::SODIUM)
endif()
