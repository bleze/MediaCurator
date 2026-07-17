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

    # Default-on "Run MediaCurator" checkbox on the finish page of an interactive
    # install. This is what resumes the app after UpdateChecker's self-update flow
    # (download installer -> run it elevated, wizard UI -> close this process),
    # since that install is no longer silent (see UpdateChecker::launchInstaller).
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "MediaCurator.exe")

    # Explicit NSIS script commands — more reliable than CPACK_PACKAGE_EXECUTABLES
    # when the exe lives in the install root rather than a bin/ subdirectory.
    #
    # The IfSilent branch only matters for a manual `installer.exe /S` run (e.g. a
    # scripted deployment) — NSIS skips the finish page (and its checkbox above)
    # entirely during a silent install, so without this branch a silent install
    # would leave the user with nothing running. UpdateChecker's own self-update
    # no longer goes through this branch, since it launches the wizard UI instead.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
      CreateDirectory '$SMPROGRAMS\\\\MediaCurator'
      CreateShortCut  '$SMPROGRAMS\\\\MediaCurator\\\\MediaCurator.lnk' '$INSTDIR\\\\MediaCurator.exe'
      CreateShortCut  '$DESKTOP\\\\MediaCurator.lnk' '$INSTDIR\\\\MediaCurator.exe'
      IfSilent 0 mc_run_done
        Exec '$INSTDIR\\\\MediaCurator.exe'
      mc_run_done:
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

    # Self-contained install tree (same convention as Chrome/Slack/VS Code .deb
    # packages) instead of scattering files under the shared /usr/bin, /usr/lib.
    # This matters because src/CMakeLists.txt now bundles Qt's own .so files
    # via linuxdeployqt — dropping unnamespaced libQt6*.so files directly into
    # /usr/lib would risk colliding with (or shadowing) other packages' copies.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/mediacurator")

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
    # No libqt6* Depends here on purpose. Qt (including the QSQLITE driver
    # plugin that used to need its own libqt6sql6-sqlite Depends entry) is now
    # bundled directly into the package by linuxdeployqt (see src/CMakeLists.txt),
    # so the app never touches the distro's Qt6 packages. Relying on apt Depends
    # meant the app linked against whatever Qt6 minor version the target distro
    # shipped, which doesn't have to match the 6.8.3 build used in CI — that
    # mismatch is what caused crashes like "libQt6Svg.so.6: version 'qt_6.8'
    # not found".
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE    "https://github.com/bleze/MediaCurator")
endif()

include(CPack)
