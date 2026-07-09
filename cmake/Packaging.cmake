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

# ── Bundle external tools (Windows only — populated by scripts/setup_tools.ps1)
if(WIN32)
    set(_tools_win "${CMAKE_SOURCE_DIR}/tools/windows")

    if(EXISTS "${_tools_win}/ffprobe.exe")
        install(PROGRAMS "${_tools_win}/ffprobe.exe"
                DESTINATION "tools/windows")
    else()
        message(WARNING
            "tools/windows/ffprobe.exe not found — run scripts/setup_tools.ps1 before packaging.")
    endif()

    if(IS_DIRECTORY "${_tools_win}/mkvtoolnix")
        install(DIRECTORY "${_tools_win}/mkvtoolnix"
                DESTINATION "tools/windows"
                USE_SOURCE_PERMISSIONS)
    else()
        message(WARNING
            "tools/windows/mkvtoolnix/ not found — run scripts/setup_tools.ps1 before packaging.")
    endif()
endif()

# ── Third-party licence notices (required by LGPL / GPL) ──────────────────────
if(EXISTS "${CMAKE_SOURCE_DIR}/resources/third_party_licenses")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/third_party_licenses/"
            DESTINATION "licenses/third_party")
endif()

# ── Windows — NSIS installer (.exe) ───────────────────────────────────────────
if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/resources/icons/app_icon.ico")
        # NSIS requires backslash paths.  CPack writes the value verbatim into
        # CPackConfig.cmake inside a set() call, so each \ must be stored as \\
        # (CMake string-escape) to survive the round-trip without a parse error.
        set(_ico "${CMAKE_SOURCE_DIR}/src/resources/icons/app_icon.ico")
        string(REPLACE "/" "\\\\" _ico "${_ico}")
        set(CPACK_NSIS_MUI_ICON    "${_ico}")
        set(CPACK_NSIS_MUI_UNIICON "${_ico}")
    endif()
    # Programs & Features icon — must point to a file that still exists when the
    # user opens Add/Remove Programs AFTER install.  Using the main exe (which
    # has the icon embedded) avoids a broken icon during or after uninstall.
    set(CPACK_NSIS_INSTALLED_ICON_NAME "MediaCurator.exe")
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
    # libqt6sql6 only pulls in the QtSql module itself — the SQLite backend
    # (QSQLITE, what DatabaseManager actually opens) ships in the separate
    # libqt6sql6-sqlite driver plugin package. Without it, QSqlDatabase has
    # no usable driver and the app fails on first launch, which looks like
    # "Qt is missing" even though every libqt6*6 package installed fine.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libqt6core6t64 | libqt6core6, \
libqt6gui6t64 | libqt6gui6, \
libqt6widgets6t64 | libqt6widgets6, \
libqt6sql6t64 | libqt6sql6, \
libqt6sql6-sqlite, \
libqt6network6t64 | libqt6network6, \
libqt6svg6")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE    "https://github.com/bleze/MediaCurator")
endif()

include(CPack)
