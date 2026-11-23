# Sanitizer builds (CI job "Sanitizers") and the libFuzzer build (CI job "Fuzz").
#   -DZPROJECT_SANITIZE=address,undefined   instrument everything; run ctest
set(ZPROJECT_SANITIZE "" CACHE STRING "Comma-separated -fsanitize= list (e.g. address,undefined)")

if(ZPROJECT_SANITIZE)
  add_compile_options(-fsanitize=${ZPROJECT_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=${ZPROJECT_SANITIZE})
  message(STATUS "zproject: sanitizers ${ZPROJECT_SANITIZE}")
endif()

# libFuzzer (fuzz/, clang only):
#   -DZPROJECT_FUZZ=ON   instrument everything for coverage, ASan and UBSan, and
#                        build the harnesses (turn tests and the GUI off)
option(ZPROJECT_FUZZ "Build the libFuzzer harnesses in fuzz/ (clang)" OFF)
if(ZPROJECT_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ZPROJECT_FUZZ needs clang (libFuzzer)")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "zproject: libFuzzer harnesses")
endif()
