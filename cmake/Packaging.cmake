# cmake/Packaging.cmake — CPack installer configuration

set(CPACK_PACKAGE_NAME            "MediaCurator")
set(CPACK_PACKAGE_VENDOR          "Bleze Software")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Smart media library curator — scan, filter, and clean your video collection")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR   "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR   "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH   "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "MediaCurator")
set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/bleze/MediaCurator")
set(CPACK_RESOURCE_FILE_LICENSE   "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_EXECUTABLES     "MediaCurator;MediaCurator")

# ── Windows — NSIS installer (.exe) ───────────────────────────────────────────
if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/resources/icons/app_icon.ico")
        set(CPACK_NSIS_MUI_ICON   "${CMAKE_SOURCE_DIR}/src/resources/icons/app_icon.ico")
        set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/src/resources/icons/app_icon.ico")
    endif()
    set(CPACK_NSIS_DISPLAY_NAME        "MediaCurator ${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME        "MediaCurator")
    set(CPACK_NSIS_URL_INFO_ABOUT      "https://github.com/bleze/MediaCurator")
    set(CPACK_NSIS_CONTACT             "mediacurator@bleze.dk")
    set(CPACK_NSIS_MODIFY_PATH         ON)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # Explicit NSIS script commands — more reliable than CPACK_PACKAGE_EXECUTABLES
    # when the exe lives in the install root rather than a bin/ subdirectory.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
      CreateDirectory '$SMPROGRAMS\\\\MediaCurator'
      CreateShortCut  '$SMPROGRAMS\\\\MediaCurator\\\\MediaCurator.lnk' '$INSTDIR\\\\MediaCurator.exe'
      CreateShortCut  '$DESKTOP\\\\MediaCurator.lnk' '$INSTDIR\\\\MediaCurator.exe'
    ")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
      Delete '$SMPROGRAMS\\\\MediaCurator\\\\MediaCurator.lnk'
      RMDir  '$SMPROGRAMS\\\\MediaCurator'
      Delete '$DESKTOP\\\\MediaCurator.lnk'
    ")

# ── macOS — DragNDrop (.dmg) ───────────────────────────────────────────────────
elseif(APPLE)
    set(CPACK_GENERATOR      "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "MediaCurator ${PROJECT_VERSION}")
    set(CPACK_DMG_FORMAT      "UDZO")

# ── Linux — Debian package (.deb) ─────────────────────────────────────────────
else()
    set(CPACK_GENERATOR "DEB")

    set(CPACK_DEBIAN_PACKAGE_NAME        "mediacurator")
    set(CPACK_DEBIAN_PACKAGE_VERSION     "${PROJECT_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER  "Jacob Pedersen <mediacurator@bleze.dk>")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
        "MediaCurator — smart video library curator\n"
        " Scans media files with ffprobe, stores metadata in SQLite, and uses\n"
        " mkvmerge to losslessly remove unwanted audio/subtitle tracks.")
    set(CPACK_DEBIAN_PACKAGE_SECTION     "video")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY    "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libqt6core6t64 | libqt6core6, \
libqt6gui6t64 | libqt6gui6, \
libqt6widgets6t64 | libqt6widgets6, \
libqt6sql6t64 | libqt6sql6, \
libqt6network6t64 | libqt6network6, \
libqt6svg6")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE    "https://github.com/bleze/MediaCurator")
endif()

include(CPack)
