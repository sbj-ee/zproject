# Warning flags for zproject's own targets (not third_party/).
#   -DZPROJECT_WERROR=ON   treat warnings as errors (CI sets this; off by
#                          default so a newer compiler doesn't break a build)
option(ZPROJECT_WERROR "Treat compiler warnings in zproject's own targets as errors" OFF)

function(zproject_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference)
  if(ZPROJECT_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
