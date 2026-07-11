set(MC_SOURCES
    main.cpp

    # Core
    core/AppSettings.cpp
    core/StorageGroupSettings.cpp
    core/DatabaseManager.cpp
    core/ExternalTools.cpp
    core/LibraryLoader.cpp
    core/UserProfile.cpp
    core/UpdateChecker.cpp

    # Scanner
    scanner/FfprobeScanner.cpp
    scanner/ScanWorker.cpp

    # UI
    ui/McFilterPanel.cpp
    ui/McLibraryPanel.cpp
    ui/McMainWindow.cpp
    ui/McTrackTableModel.cpp
    ui/McFileListModel.cpp
    ui/McCardDelegate.cpp
    ui/McLanguageFlags.cpp
    ui/McLegendDialog.cpp
    ui/McOnboardingDialog.cpp
    ui/RangeSlider.cpp
    ui/McSettingsDialog.cpp
    ui/McJobPanel.cpp
    ui/McJobListModel.cpp
    ui/McManageFoldersDialog.cpp
    ui/McPreviewDialog.cpp
    ui/McJobReviewDialog.cpp
    ui/McBulkSummaryDialog.cpp
    ui/McWhatIfDialog.cpp
    ui/McJobStatsBar.cpp
    ui/McGbGoalDialog.cpp
    ui/McGoalProgressBar.cpp
    ui/ImdbSearchDialog.cpp
    ui/McSubtitleDownloadDialog.cpp
    ui/McCalibrationDialog.cpp
    ui/SvgIcon.cpp

    # Engine
    engine/RuleEngine.cpp
    engine/ActionEngine.cpp
    engine/AnalyzeWorker.cpp
    engine/SimulateWorker.cpp
    engine/JobQueue.cpp
    engine/RemuxJob.cpp
    engine/PosterManager.cpp
    engine/SubtitleManager.cpp
    engine/OpenSubtitlesClient.cpp

    # Classifier
    classifier/RegexClassifier.cpp

    # Scanner extras
    scanner/OriginalLanguageDetector.cpp
    scanner/NfoParser.cpp

)

set(MC_HEADERS
    # Core
    ../include/core/DatabaseManager.h
    ../include/core/AppSettings.h
    ../include/core/LibraryLoader.h
    ../include/core/UserProfile.h
    ../include/core/ExternalTools.h
    ../include/core/UpdateChecker.h

    # Scanner
    ../include/scanner/FfprobeScanner.h
    ../include/scanner/ScanWorker.h
    ../include/scanner/OriginalLanguageDetector.h
    ../include/scanner/NfoParser.h

    # Engine
    ../include/engine/RuleEngine.h
    ../include/engine/ActionEngine.h
    ../include/engine/AnalyzeWorker.h
    ../include/engine/SimulateWorker.h
    ../include/engine/JobQueue.h
    ../include/engine/RemuxJob.h
    ../include/engine/TrackDecision.h
    ../include/engine/PosterManager.h
    ../include/engine/SubtitleManager.h
    ../include/engine/OpenSubtitlesClient.h

    # Classifier
    ../include/classifier/TrackClassifier.h
    ../include/classifier/RegexClassifier.h

    # UI
    ../include/ui/McMainWindow.h
    ../include/ui/McTrackTableModel.h
    ../include/ui/McFileListModel.h
    ../include/ui/McCardDelegate.h
    ../include/ui/McLanguageFlags.h
    ../include/ui/McLegendDialog.h
    ../include/ui/McOnboardingDialog.h
    ../include/ui/McFileCardDelegate.h
    ../include/ui/McJobCardDelegate.h
    ../include/ui/RangeSlider.h
    ../include/ui/McLibraryPanel.h
    ../include/ui/McFilterPanel.h
    ../include/ui/McJobPanel.h
    ../include/ui/McJobListModel.h
    ../include/ui/McManageFoldersDialog.h
    ../include/ui/McPreviewDialog.h
    ../include/ui/McJobReviewDialog.h
    ../include/ui/McBulkSummaryDialog.h
    ../include/ui/McWhatIfDialog.h
    ../include/ui/McJobStatsBar.h
    ../include/ui/McGbGoalDialog.h
    ../include/ui/McGoalProgressBar.h
    ../include/ui/McSettingsDialog.h
    ../include/ui/ImdbSearchDialog.h
    ../include/ui/McSubtitleDownloadDialog.h
    ../include/ui/McCalibrationDialog.h
    ../include/ui/SvgIcon.h
    ../include/ui/McFlagRowWidget.h
)

if(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.ico")
        list(APPEND MC_SOURCES resources/app.rc)
    else()
        message(STATUS "MediaCurator: app_icon.ico not found — exe will have no embedded icon. "
                       "Place app_icon.ico in src/resources/icons/ to enable it.")
    endif()
endif()

qt_add_executable(MediaCurator
    WIN32           # Windows: no console window in release
    MACOSX_BUNDLE   # macOS: produce a .app bundle
    ${MC_SOURCES}
    ${MC_HEADERS}
)

set_target_properties(MediaCurator PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME        "MediaCurator"
    MACOSX_BUNDLE_GUI_IDENTIFIER     "dk.bleze.mediacurator"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION       "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT          "© 2026 Jacob Pedersen / Bleze Software"
)

if(APPLE)
    # qt_add_executable(MACOSX_BUNDLE) sets INSTALL_RPATH to
    # "@executable_path/../Frameworks" on this target. With
    # BUILD_WITH_INSTALL_RPATH left at its default (FALSE), CMake tries to
    # rewrite that rpath itself during `cmake --install` (i.e. CPack's
    # "Install project" step) — deleting the build-tree rpath that points at
    # the Qt SDK's lib/ directory and adding the install one. But macdeployqt
    # already performed that exact rewrite as a POST_BUILD step, so by install
    # time the add is a duplicate and the delete target no longer exists,
    # which install_name_tool rejects. That failed rewrite leaves the staged
    # .app inconsistent, which is what makes the later `hdiutil create` step
    # fail with "Resource busy". Setting BUILD_WITH_INSTALL_RPATH here makes
    # the linker embed the install rpath directly at build time, so CMake has
    # nothing left to rewrite at install time.
    set_target_properties(MediaCurator PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
endif()

if(UNIX AND NOT APPLE)
    # Only force the install rpath when linuxdeployqt is actually available to
    # populate "../lib" (see the POST_BUILD block further down). Forcing it
    # unconditionally would break plain local builds on machines without
    # linuxdeployqt installed: CMake's default build-tree rpath (pointing at
    # the Qt SDK's own lib dir) is what makes running the freshly built binary
    # work at all in that case, and BUILD_WITH_INSTALL_RPATH would discard it.
    find_program(LINUXDEPLOYQT linuxdeployqt)
    find_program(QT_QMAKE_EXE NAMES qmake6 qmake HINTS "${Qt6_DIR}/../../../bin")
    if(LINUXDEPLOYQT AND QT_QMAKE_EXE)
        # Same rationale as the APPLE block above: bake the install rpath in
        # at build time so the linuxdeployqt POST_BUILD run (below) and any
        # later install-time rpath rewrite don't fight each other.
        # "$ORIGIN/../lib" matches where linuxdeployqt places the bundled Qt
        # .so files relative to the executable (bin/MediaCurator -> ../lib).
        set_target_properties(MediaCurator PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN/../lib"
        )
    endif()
endif()

target_include_directories(MediaCurator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(MediaCurator PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Network
    Qt6::Svg
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(MediaCurator PRIVATE Ole32)
endif()

qt_add_resources(MediaCurator "mc_resources"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        resources/icons/folder_open.svg
        resources/icons/manage_search.svg
        resources/icons/refresh.svg
        resources/icons/settings.svg
        resources/icons/playlist_add_check.svg
        resources/icons/add_to_queue.svg
        resources/icons/delete.svg
        resources/icons/terminal.svg
        resources/icons/play_arrow.svg
        resources/icons/pause.svg
        resources/icons/stop_circle.svg
        resources/icons/app_icon.svg
        resources/icons/visibility.svg
        resources/icons/link.svg
        resources/icons/logout.svg
        resources/icons/undo.svg
        resources/icons/imdb.png
        resources/icons/vlc.svg
        resources/icons/dollar.svg
        resources/icons/lightning_qr.png
        resources/icons/paypal_qr.png
        resources/icons/block.svg
        resources/icons/visibility_off.svg
        resources/icons/legend_toggle.svg
        resources/icons/copy.svg
        resources/icons/closed_caption.svg
        resources/icons/star.svg
        resources/icons/push_pin.svg
        resources/icons/translate.svg
        resources/icons/chat.svg
        resources/icons/system_update.svg
)

# Country flags for track-badge language icons (lipis/flag-icons, MIT —
# see resources/flags/LICENSE.flag-icons.txt). Globbed so adding a flag is
# just dropping the SVG in; re-run CMake configure after adding files.
file(GLOB MC_FLAG_SVGS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/flags/*.svg")
qt_add_resources(MediaCurator "mc_flags"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES ${MC_FLAG_SVGS}
)

# Copy tools/ directory next to the executable after build.
# tools/ is gitignored (populated by scripts/setup_tools.ps1) so it won't exist
# on a fresh checkout or CI runner — guard to avoid a hard build failure.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
    add_custom_command(TARGET MediaCurator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/tools"
            "$<TARGET_FILE_DIR:MediaCurator>/tools"
        COMMENT "Copying bundled tools to output directory (ffprobe + mkvmerge + mkvpropedit)"
    )
endif()

# Copy data/ directory (classifier rules, etc.)
if(EXISTS "${CMAKE_SOURCE_DIR}/data")
    add_custom_command(TARGET MediaCurator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:MediaCurator>/data"
        COMMENT "Copying data files to output directory"
    )
endif()

# Windows: deploy Qt DLLs automatically (requires windeployqt in PATH)
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET MediaCurator POST_BUILD
            COMMAND "${WINDEPLOYQT}"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:MediaCurator>"
            COMMENT "Running windeployqt to copy Qt DLLs"
        )
    endif()
endif()

# macOS: bundle Qt frameworks into the .app
if(APPLE)
    find_program(MACDEPLOYQT macdeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(MACDEPLOYQT)
        add_custom_command(TARGET MediaCurator POST_BUILD
            COMMAND "${MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:MediaCurator>"
            COMMENT "Running macdeployqt to bundle Qt frameworks"
        )
    endif()
endif()

# Linux: bundle Qt shared libraries + plugins so the package never falls back
# to (and mismatches with) whatever Qt6 the target distro's repos happen to
# ship. Without this, the executable links against libQt6*.so.6 by SONAME
# only, so at runtime it can load an ABI-incompatible system Qt build — e.g.
# "libQt6Svg.so.6: version 'qt_6.8' not found" when the distro's Qt6 predates
# the one this was built against.
if(UNIX AND NOT APPLE)
    # LINUXDEPLOYQT / QT_QMAKE_EXE were probed above (guarding the rpath block).
    if(LINUXDEPLOYQT AND QT_QMAKE_EXE)
        add_custom_command(TARGET MediaCurator POST_BUILD
            COMMAND "${LINUXDEPLOYQT}"
                "$<TARGET_FILE:MediaCurator>"
                -bundle-non-qt-libs
                -no-translations
                "-qmake=${QT_QMAKE_EXE}"
            COMMENT "Running linuxdeployqt to bundle Qt libraries"
        )
    else()
        message(WARNING "linuxdeployqt (and/or qmake) not found — MediaCurator "
                         "will link against system Qt6 only; the .deb will not "
                         "be self-contained and may fail on machines whose Qt6 "
                         "differs from this build's.")
    endif()
endif()

# ── Install rules (used by CPack and cmake --install) ─────────────────────────
if(WIN32)
    install(TARGETS MediaCurator RUNTIME DESTINATION .)
    # Re-run windeployqt against the staged install so the installer gets Qt DLLs
    if(WINDEPLOYQT)
        install(CODE "
            message(STATUS \"Running windeployqt for install...\")
            execute_process(COMMAND
                \"${WINDEPLOYQT}\"
                    --no-translations
                    --no-system-d3d-compiler
                    \"\${CMAKE_INSTALL_PREFIX}/MediaCurator.exe\"
            )
        ")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/" DESTINATION tools)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/data")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION data)
    endif()
elseif(APPLE)
    install(TARGETS MediaCurator BUNDLE DESTINATION .)
    # macdeployqt already ran at POST_BUILD and embedded Qt frameworks into the
    # .app in the build directory.  install(TARGETS BUNDLE) copies the entire
    # bundle recursively, so the staged .app already has Frameworks/PlugIns.
    # Do not run macdeployqt again here — see the BUILD_WITH_INSTALL_RPATH
    # comment above for why a second rpath rewrite (whether from macdeployqt
    # or from CMake's own install-time rpath fixup) breaks the staged bundle
    # and cascades into "hdiutil: Resource busy" when CPack tries to package it.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/"
            DESTINATION MediaCurator.app/Contents/MacOS/tools)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/data")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/"
            DESTINATION MediaCurator.app/Contents/MacOS/data)
    endif()
    # Ad-hoc codesign as the LAST step that touches the bundle — adding files
    # after signing invalidates the signature, so this must run after the
    # tools/ and data/ copies above. Without any signature at all (which is
    # what a plain Ninja/clang build produces — Xcode's build system is the
    # only thing that ad-hoc signs automatically), Gatekeeper refuses to even
    # launch the app on Apple Silicon once it's downloaded and quarantined,
    # reporting it as "damaged" instead of the normal unidentified-developer
    # warning. This is not a substitute for a real Developer ID + notarization
    # (users still need Right-click > Open / "Open Anyway" once) but it makes
    # the app launchable at all.
    find_program(CODESIGN codesign)
    if(CODESIGN)
        install(CODE "
            message(STATUS \"Ad-hoc codesigning MediaCurator.app...\")
            execute_process(COMMAND
                \"${CODESIGN}\" --force --deep --sign - \"\${CMAKE_INSTALL_PREFIX}/MediaCurator.app\"
                RESULT_VARIABLE _codesign_result
            )
            if(NOT _codesign_result EQUAL 0)
                message(FATAL_ERROR \"codesign failed with exit code \${_codesign_result}\")
            endif()
        ")
    else()
        message(WARNING "codesign not found — MediaCurator.app will be unsigned and may refuse to launch on Apple Silicon after download.")
    endif()
else()
    install(TARGETS MediaCurator RUNTIME DESTINATION bin)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/" DESTINATION lib/MediaCurator/tools)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/data")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION lib/MediaCurator/data)
    endif()
    # Re-run linuxdeployqt against the staged install so the packaged .deb
    # gets its own copy of Qt's .so files + plugins (platforms, sqldrivers,
    # iconengines, etc.) under CMAKE_INSTALL_PREFIX/lib and .../plugins,
    # matching the "$ORIGIN/../lib" INSTALL_RPATH set above. This mirrors the
    # windeployqt re-run done for the Windows installer above.
    if(LINUXDEPLOYQT AND QT_QMAKE_EXE)
        # CPack's DEB/RPM generators do the "install project" step with
        # DESTDIR set to a temporary staging directory rather than actually
        # writing to CMAKE_INSTALL_PREFIX (/opt/mediacurator) — so the real
        # on-disk path to operate on is $ENV{DESTDIR} + the prefix, not the
        # prefix alone. install(TARGETS/FILES/DIRECTORY) handle this
        # automatically; a raw execute_process() here does not.
        install(CODE "
            message(STATUS \"Running linuxdeployqt for install...\")
            execute_process(
                COMMAND \"${LINUXDEPLOYQT}\"
                    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/MediaCurator\"
                    -bundle-non-qt-libs
                    -no-translations
                    \"-qmake=${QT_QMAKE_EXE}\"
                RESULT_VARIABLE _linuxdeployqt_result
            )
            if(NOT _linuxdeployqt_result EQUAL 0)
                message(FATAL_ERROR \"linuxdeployqt failed with exit code \${_linuxdeployqt_result}\")
            endif()
        ")
    endif()
    # .desktop entry + icon so the app shows up in the desktop environment's
    # application menu after installing the .deb (desktop-file-utils' dpkg
    # trigger on /usr/share/applications picks these up automatically).
    install(FILES "${CMAKE_SOURCE_DIR}/resources/linux/mediacurator.desktop"
            DESTINATION share/applications)
    install(FILES "${CMAKE_SOURCE_DIR}/src/resources/icons/app_icon.svg"
            DESTINATION share/icons/hicolor/scalable/apps
            RENAME mediacurator.svg)
    # CPACK_PACKAGING_INSTALL_PREFIX (cmake/Packaging.cmake) puts the app in
    # its own /opt/mediacurator tree so the bundled Qt libs never land in the
    # shared /usr/lib. mediacurator.desktop's Exec= still points at
    # /usr/bin/MediaCurator, so symlink that name into place rather than
    # editing the desktop file.
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        file(CREATE_LINK \"\${CMAKE_INSTALL_PREFIX}/bin/MediaCurator\"
             \"\$ENV{DESTDIR}/usr/bin/MediaCurator\"
             SYMBOLIC)
    ")
endif()
