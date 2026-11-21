# Install rules + CPack (SPEC.md §20). Version comes solely from
# project(zproject VERSION ...) -> PROJECT_VERSION. Two Debian packages from
# one build, one per CPack component:
#   zproject_<ver>_amd64.deb    component "desktop": zproject + zproject-cli
#   zprojectd_<ver>_amd64.deb   component "server":  zprojectd (no Qt Widgets)
# The server package carries the systemd units (zprojectd.service and the
# 03:17 zprojectd-backup.timer), /etc/zproject/zprojectd.conf (a conffile with
# placeholders) and a postinst that creates the zproject user; it does not
# enable or start anything.
#   zproject-<ver>-Darwin.dmg   macOS arm64: zproject.app with zproject-cli in
#                               Contents/MacOS, Qt deployed, ad-hoc signed

include(GNUInstallDirs)

set(CPACK_PACKAGE_NAME "zproject")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Stephen B. Johnson")
set(CPACK_PACKAGE_CONTACT "Stephen B. Johnson <49662809+sbj-ee@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sbj-ee/zproject")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Project management with a Gantt chart, critical path and baselines (Qt 6)")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

if(UNIX AND NOT APPLE)
  if(ZPROJECT_BUILD_GUI)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/zproject.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT desktop)
    foreach(_sz 16 24 32 48 64 128 256 512)
      install(FILES ${CMAKE_SOURCE_DIR}/assets/icon/zproject-${_sz}.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps
              RENAME zproject.png COMPONENT desktop)
    endforeach()
    install(FILES ${CMAKE_SOURCE_DIR}/assets/icon/zproject.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT desktop)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/zproject RENAME copyright COMPONENT desktop)
  endif()
  install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
          DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/zprojectd RENAME copyright COMPONENT server)
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/systemd/zprojectd.service
                ${CMAKE_SOURCE_DIR}/packaging/systemd/zprojectd-backup.service
                ${CMAKE_SOURCE_DIR}/packaging/systemd/zprojectd-backup.timer
          DESTINATION lib/systemd/system COMPONENT server)
  # Absolute on purpose: Debian conffiles live in /etc whatever the prefix.
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/zprojectd.conf
          DESTINATION /etc/zproject
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ COMPONENT server)
  file(WRITE ${CMAKE_BINARY_DIR}/packaging/conffiles "/etc/zproject/zprojectd.conf\n")

  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEB_COMPONENT_INSTALL ON)
  if(ZPROJECT_BUILD_GUI)
    set(CPACK_COMPONENTS_ALL desktop server)
  else()
    set(CPACK_COMPONENTS_ALL server)
  endif()
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
  # dpkg-shlibdeps derives Depends (libqt6core6, libqt6network6, ...) from the
  # binaries, so the packages match the distro Qt they were built against.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

  set(CPACK_DEBIAN_DESKTOP_PACKAGE_NAME "zproject")
  set(CPACK_DEBIAN_DESKTOP_FILE_NAME "zproject_${PROJECT_VERSION}_amd64.deb")
  set(CPACK_DEBIAN_DESKTOP_PACKAGE_SECTION "x11")
  # Qt loads OpenSSL at run time for https:// (shlibdeps cannot see it).
  set(CPACK_DEBIAN_DESKTOP_PACKAGE_DEPENDS "libssl3t64 | libssl3")
  set(CPACK_DEBIAN_DESKTOP_PACKAGE_SUGGESTS "libsecret-tools")
  set(CPACK_COMPONENT_DESKTOP_DESCRIPTION
      "zproject desktop app and zproject-cli.\nGantt, WBS, critical path and baselines; a client of zprojectd.")

  set(CPACK_DEBIAN_SERVER_PACKAGE_NAME "zprojectd")
  set(CPACK_DEBIAN_SERVER_FILE_NAME "zprojectd_${PROJECT_VERSION}_amd64.deb")
  set(CPACK_DEBIAN_SERVER_PACKAGE_SECTION "net")
  set(CPACK_DEBIAN_SERVER_PACKAGE_DEPENDS "adduser")
  set(CPACK_DEBIAN_SERVER_PACKAGE_CONTROL_EXTRA
      "${CMAKE_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_SOURCE_DIR}/packaging/debian/prerm;${CMAKE_SOURCE_DIR}/packaging/debian/postrm;${CMAKE_BINARY_DIR}/packaging/conffiles")
  set(CPACK_DEBIAN_SERVER_PACKAGE_CONTROL_STRICT_PERMISSION ON)
  set(CPACK_COMPONENT_SERVER_DESCRIPTION
      "zproject server.\nHeadless zprojectd (Qt Core/Network, SQLite) serving /v1 and /mcp on loopback behind tailscale serve.")
  include(CPack)
  message(STATUS "zproject: CPack configured for .deb (version ${PROJECT_VERSION})")
elseif(APPLE AND ZPROJECT_BUILD_GUI)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "zproject ${PROJECT_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "zproject-${PROJECT_VERSION}-Darwin")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
  if(DEFINED ZPROJECT_ICNS_FILE)
    install(FILES "${ZPROJECT_ICNS_FILE}" DESTINATION zproject.app/Contents/Resources COMPONENT desktop)
  endif()
  install(SCRIPT "${CMAKE_BINARY_DIR}/MacDeploy.cmake" COMPONENT desktop)
  include(CPack)
  message(STATUS "zproject: CPack configured for .dmg (version ${PROJECT_VERSION})")
endif()
