# macOS: build zproject as a real zproject.app bundle (Apple Silicon only).
# Included from the top-level CMakeLists.txt after the zproject target exists.
# Ported from ztodo (same signing approach: ad-hoc, no hardened runtime).
#   - Info.plist from cmake/Info.plist.in (CFBundleIdentifier comes from
#     MACOSX_BUNDLE_GUI_IDENTIFIER, which is set on the target below)
#   - zproject.icns generated from assets/icon/*.png with iconutil
#   - zproject-cli inside the bundle (Contents/MacOS), deployed with it
# Qt frameworks and plugins are copied at install time by
# macdeployqt (cmake/MacDeploy.cmake.in, run by `cpack -G DragNDrop`).

set(ZPROJECT_BUNDLE_ID "ee.sbj.zproject")

set_target_properties(zproject PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${ZPROJECT_BUNDLE_ID}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  OUTPUT_NAME "zproject"
)

# --- App icon: iconset from PNG art -> zproject.icns --------------------
find_program(ZPROJECT_ICONUTIL iconutil REQUIRED)
set(_icons "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon")
set(_iconset "${CMAKE_CURRENT_BINARY_DIR}/zproject.iconset")
set(_icns "${CMAKE_CURRENT_BINARY_DIR}/zproject.icns")
set(_iconmap
  icon_16x16:16 icon_16x16@2x:32 icon_32x32:32 icon_32x32@2x:64
  icon_128x128:128 icon_128x128@2x:256 icon_256x256:256 icon_256x256@2x:512
  icon_512x512:512 icon_512x512@2x:1024)
set(_copy_cmds)
set(_icon_deps)
foreach(_pair IN LISTS _iconmap)
  string(REPLACE ":" ";" _kv "${_pair}")
  list(GET _kv 0 _name)
  list(GET _kv 1 _size)
  list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy
       "${_icons}/zproject-${_size}.png" "${_iconset}/${_name}.png")
  list(APPEND _icon_deps "${_icons}/zproject-${_size}.png")
endforeach()
list(REMOVE_DUPLICATES _icon_deps)
add_custom_command(
  OUTPUT "${_icns}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_iconset}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset}"
  ${_copy_cmds}
  COMMAND "${ZPROJECT_ICONUTIL}" -c icns "${_iconset}" -o "${_icns}"
  DEPENDS ${_icon_deps}
  COMMENT "Generating zproject.icns"
  VERBATIM)
# zproject lives in app/; add_custom_command(OUTPUT) does not attach to
# targets defined in another CMakeLists. Force generation with a helper
# target.
add_custom_target(zproject-icns ALL DEPENDS "${_icns}")
add_dependencies(zproject zproject-icns)
target_sources(zproject PRIVATE "${_icns}")
set_source_files_properties("${_icns}" PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources
  GENERATED TRUE)

# --- Deployment (runs at install time, i.e. inside `cpack -G DragNDrop`) ---
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qtbin "${_qmake}" DIRECTORY)
find_program(ZPROJECT_MACDEPLOYQT NAMES macdeployqt macdeployqt6
  HINTS "${_qtbin}" REQUIRED)
set(ZPROJECT_ICNS_FILE "${_icns}")
# Extra -libpath entries so macdeployqt can resolve Homebrew Qt transitive deps.
set(ZPROJECT_EXTRA_LIBPATHS "")
if(DEFINED ENV{HOMEBREW_PREFIX})
  list(APPEND ZPROJECT_EXTRA_LIBPATHS "$ENV{HOMEBREW_PREFIX}/lib")
  if(EXISTS "$ENV{HOMEBREW_PREFIX}/opt/qt/lib")
    list(APPEND ZPROJECT_EXTRA_LIBPATHS "$ENV{HOMEBREW_PREFIX}/opt/qt/lib")
  endif()
endif()
get_filename_component(_qt_prefix "${_qtbin}" DIRECTORY) # .../qt/bin -> .../qt
if(EXISTS "${_qt_prefix}/lib")
  list(APPEND ZPROJECT_EXTRA_LIBPATHS "${_qt_prefix}/lib")
endif()
list(REMOVE_DUPLICATES ZPROJECT_EXTRA_LIBPATHS)
# Space-separated for MacDeploy.cmake.in foreach(IN ITEMS ...).
string(REPLACE ";" " " ZPROJECT_EXTRA_LIBPATHS "${ZPROJECT_EXTRA_LIBPATHS}")
set(ZPROJECT_CHECK_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos/check-bundle.sh")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacDeploy.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/MacDeploy.cmake" @ONLY)
message(STATUS "zproject: macOS app bundle ${ZPROJECT_BUNDLE_ID}, macdeployqt ${ZPROJECT_MACDEPLOYQT}")
