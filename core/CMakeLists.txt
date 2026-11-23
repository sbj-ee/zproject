# zproject_core: model, validation, scheduling engine and parsers (SPEC.md
# §4.1). No GUI and no network: Qt Core only.
add_library(zproject_core STATIC
  Error.hpp
  Holidays.cpp Holidays.hpp
  Id.cpp Id.hpp
  Model.cpp Model.hpp
  Rollup.cpp Rollup.hpp
  UpdateSigningKey.cpp UpdateSigningKey.hpp
  Version.cpp Version.hpp
  WorkCalendar.cpp WorkCalendar.hpp)
target_include_directories(zproject_core PUBLIC
  "${CMAKE_SOURCE_DIR}"
  "${ZPROJECT_GENERATED_DIR}")
target_link_libraries(zproject_core PUBLIC Qt6::Core)
zproject_set_warnings(zproject_core)
