# Sanitizer builds (CI job "Sanitizers"); fuzzing arrives with the first parser.
#   -DZPROJECT_SANITIZE=address,undefined   instrument everything; run ctest
set(ZPROJECT_SANITIZE "" CACHE STRING "Comma-separated -fsanitize= list (e.g. address,undefined)")

if(ZPROJECT_SANITIZE)
  add_compile_options(-fsanitize=${ZPROJECT_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=${ZPROJECT_SANITIZE})
  message(STATUS "zproject: sanitizers ${ZPROJECT_SANITIZE}")
endif()
