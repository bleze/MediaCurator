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
    ui/RangeSlider.cpp
    ui/McSettingsDialog.cpp
    ui/McJobPanel.cpp
    ui/McJobListModel.cpp
    ui/McManageFoldersDialog.cpp
    ui/McPreviewDialog.cpp
    ui/McBulkSummaryDialog.cpp
    ui/McJobStatsBar.cpp
    ui/ImdbSearchDialog.cpp
    ui/SvgIcon.cpp

    # Engine
    engine/RuleEngine.cpp
    engine/ActionEngine.cpp
    engine/AnalyzeWorker.cpp
    engine/JobQueue.cpp
    engine/RemuxJob.cpp
    engine/PosterManager.cpp

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
    ../include/engine/JobQueue.h
    ../include/engine/RemuxJob.h
    ../include/engine/TrackDecision.h
    ../include/engine/PosterManager.h

    # Classifier
    ../include/classifier/TrackClassifier.h
    ../include/classifier/RegexClassifier.h

    # UI
    ../include/ui/McMainWindow.h
    ../include/ui/McTrackTableModel.h
    ../include/ui/McFileListModel.h
    ../include/ui/McCardDelegate.h
    ../include/ui/McFileCardDelegate.h
    ../include/ui/McJobCardDelegate.h
    ../include/ui/RangeSlider.h
    ../include/ui/McLibraryPanel.h
    ../include/ui/McFilterPanel.h
    ../include/ui/McJobPanel.h
    ../include/ui/McJobListModel.h
    ../include/ui/McManageFoldersDialog.h
    ../include/ui/McPreviewDialog.h
    ../include/ui/McBulkSummaryDialog.h
    ../include/ui/McJobStatsBar.h
    ../include/ui/McSettingsDialog.h
    ../include/ui/ImdbSearchDialog.h
    ../include/ui/SvgIcon.h
)

if(WIN32)
    list(APPEND MC_SOURCES resources/app.rc)
endif()

qt_add_executable(MediaCurator
    WIN32    # Windows: no console window in release
    ${MC_SOURCES}
    ${MC_HEADERS}
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
)

# Copy tools/ directory next to the executable after build
add_custom_command(TARGET MediaCurator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/tools"
        "$<TARGET_FILE_DIR:MediaCurator>/tools"
    COMMENT "Copying bundled tools to output directory (ffprobe + mkvmerge are the active tools)"
)

# Copy data/ directory (classifier rules, etc.)
add_custom_command(TARGET MediaCurator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data"
        "$<TARGET_FILE_DIR:MediaCurator>/data"
    COMMENT "Copying data files to output directory"
)

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

install(TARGETS MediaCurator
    RUNTIME DESTINATION bin
)
