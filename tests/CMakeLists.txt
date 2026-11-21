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

# Every binary reports the single project() version.
add_test(NAME zprojectd_version COMMAND zprojectd --version)
set_tests_properties(zprojectd_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^zprojectd ${PROJECT_VERSION}\napiVersion 1\n$")
add_test(NAME zprojectd_refuses_non_loopback COMMAND zprojectd --listen 0.0.0.0:8788)
set_tests_properties(zprojectd_refuses_non_loopback PROPERTIES
  PASS_REGULAR_EXPRESSION "only loopback addresses are allowed")
add_test(NAME zproject_cli_version COMMAND zproject-cli --version)
set_tests_properties(zproject_cli_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^zproject-cli ${PROJECT_VERSION}\n$")
if(ZPROJECT_BUILD_GUI)
  add_test(NAME zproject_version COMMAND zproject --version)
  set_tests_properties(zproject_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^zproject ${PROJECT_VERSION}\n$")
endif()
