set(MC_SOURCES
    main.cpp

    # Core
    core/AppSettings.cpp
    core/DatabaseManager.cpp
    core/ExternalTools.cpp
    core/LibraryLoader.cpp
    core/UserProfile.cpp

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
    ui/ImdbSearchDialog.cpp
    ui/McSubtitleDownloadDialog.cpp
    ui/SvgIcon.cpp

    # Engine
    engine/RuleEngine.cpp
    engine/ActionEngine.cpp
    engine/AnalyzeWorker.cpp
    engine/SimulateWorker.cpp
    engine/JobQueue.cpp
    engine/RemuxJob.cpp
    engine/PosterManager.cpp
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
    ../include/ui/McSettingsDialog.h
    ../include/ui/ImdbSearchDialog.h
    ../include/ui/McSubtitleDownloadDialog.h
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
    MACOSX_BUNDLE_COPYRIGHT          "© 2024 Jacob Pedersen / Bleze Software"
)

target_include_directories(MediaCurator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
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
        resources/icons/star.svg
        resources/icons/push_pin.svg
        resources/icons/translate.svg
        resources/icons/chat.svg
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
        COMMENT "Copying bundled tools to output directory (ffprobe + mkvmerge)"
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
    # Running macdeployqt a second time here causes install_name_tool to error
    # on the duplicate @executable_path/../Frameworks RPATH and leaves the
    # staging directory in a state that hdiutil refuses to package.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/"
            DESTINATION MediaCurator.app/Contents/MacOS/tools)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/data")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/"
            DESTINATION MediaCurator.app/Contents/MacOS/data)
    endif()
else()
    install(TARGETS MediaCurator RUNTIME DESTINATION bin)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/" DESTINATION lib/MediaCurator/tools)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/data")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION lib/MediaCurator/data)
    endif()
endif()
