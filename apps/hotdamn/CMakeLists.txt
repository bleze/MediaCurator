cmake_minimum_required(VERSION 3.25)

# Hot DAMN NFO Viewer — the scene NFO viewer from MediaCurator as a standalone
# app. Its own CMake project (own version, own installer) that shares the NFO
# engine in libs/nfoview with MediaCurator. Configure from this directory:
#   cmake -S apps/hotdamn -B apps/hotdamn/build -DQt6_DIR=<Qt>/lib/cmake/Qt6
project(HotDamn
    VERSION 0.2.0
    DESCRIPTION "Hot DAMN NFO Viewer — scene .nfo files with their ASCII art rendered right"
    HOMEPAGE_URL "https://github.com/bleze/MediaCurator"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options(/external:anglebrackets /external:W0)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

set(HOTDAMN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${HOTDAMN_REPO_ROOT}/libs/nfoview" "${CMAKE_BINARY_DIR}/nfoview")

set(HOTDAMN_SOURCES
    src/main.cpp
    src/McHotDamnWindow.cpp
    src/McHotDamnUpdater.cpp
    include/McHotDamnWindow.h
    include/McHotDamnUpdater.h
    include/HotDamnSettings.h
)
if(WIN32)
    list(APPEND HOTDAMN_SOURCES resources/hotdamn.rc)
endif()

qt_add_executable(HotDamn WIN32 ${HOTDAMN_SOURCES})

target_include_directories(HotDamn PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(HotDamn PRIVATE HOTDAMN_VERSION="${PROJECT_VERSION}")
target_link_libraries(HotDamn PRIVATE nfoview)
if(MSVC)
    target_compile_options(HotDamn PRIVATE /W4 /WX-)
endif()

qt_add_resources(HotDamn "hotdamn_resources"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES resources/hotdamn.png
)

# ── Qt deployment + install ───────────────────────────────────────────────────
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    # --no-opengl-sw: skips the ~20 MB software OpenGL fallback, which a plain
    # Widgets app never loads.
    set(HOTDAMN_DEPLOY_ARGS --no-translations --no-system-d3d-compiler --no-opengl-sw)
    if(WINDEPLOYQT)
        add_custom_command(TARGET HotDamn POST_BUILD
            COMMAND "${WINDEPLOYQT}" ${HOTDAMN_DEPLOY_ARGS} "$<TARGET_FILE:HotDamn>"
            COMMENT "Running windeployqt to copy Qt DLLs"
        )
    endif()

    install(TARGETS HotDamn RUNTIME DESTINATION .)
    if(WINDEPLOYQT)
        string(REPLACE ";" " " _deploy_args "${HOTDAMN_DEPLOY_ARGS}")
        install(CODE "
            message(STATUS \"Running windeployqt for install...\")
            execute_process(COMMAND
                \"${WINDEPLOYQT}\" ${_deploy_args}
                    \"\${CMAKE_INSTALL_PREFIX}/HotDamn.exe\"
            )
        ")
    endif()
    install(FILES "${HOTDAMN_REPO_ROOT}/LICENSE" DESTINATION licenses)
else()
    install(TARGETS HotDamn RUNTIME DESTINATION bin)
endif()

# ── Installer (Windows / NSIS) ────────────────────────────────────────────────
# Only Windows is packaged so far: the point of the installer is the .nfo file
# association, and that part is platform-specific.
if(WIN32)
    # Verbatim so the NSIS snippets below reach the installer script exactly as
    # written — no hand-doubled backslashes.
    set(CPACK_VERBATIM_VARIABLES TRUE)

    set(CPACK_GENERATOR               "NSIS")
    set(CPACK_PACKAGE_NAME            "HotDamn")
    set(CPACK_PACKAGE_VENDOR          "Bleze Software")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hot DAMN NFO Viewer")
    set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VERSION_MAJOR   "${PROJECT_VERSION_MAJOR}")
    set(CPACK_PACKAGE_VERSION_MINOR   "${PROJECT_VERSION_MINOR}")
    set(CPACK_PACKAGE_VERSION_PATCH   "${PROJECT_VERSION_PATCH}")
    set(CPACK_PACKAGE_FILE_NAME       "HotDamn-Setup-${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "HotDamn")
    set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/bleze/MediaCurator")
    set(CPACK_RESOURCE_FILE_LICENSE   "${HOTDAMN_REPO_ROOT}/LICENSE")
    set(CPACK_PACKAGE_EXECUTABLES     "HotDamn;Hot DAMN NFO Viewer")

    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/hotdamn.ico" _ico)
    set(CPACK_NSIS_MUI_ICON            "${_ico}")
    set(CPACK_NSIS_MUI_UNIICON         "${_ico}")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "HotDamn.exe")
    set(CPACK_NSIS_DISPLAY_NAME        "Hot DAMN NFO Viewer ${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME        "Hot DAMN NFO Viewer")
    set(CPACK_NSIS_URL_INFO_ABOUT      "https://github.com/bleze/MediaCurator")
    # The exe sits in $INSTDIR, not $INSTDIR/bin (see cmake/Packaging.cmake in
    # the repo root for what goes wrong when this is left at its default).
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN  "HotDamn.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)

    # File association.
    #
    # What an installer can and cannot do here: it registers the app as able to
    # open .nfo/.diz (ProgID HotDamn.nfo, OpenWithProgids, Capabilities), which
    # puts it in "Open with" and in Settings > Default apps. It cannot make the
    # app the default. Windows 10/11 only accept a default the user picked:
    # with a second viewer registered and no choice made yet, Explorer asks
    # "How do you want to open this file?" on the next double-click instead of
    # using the machine-wide default written below. So the installer asks, and
    # on yes opens the Default apps page at this app's entry to finish the job.
    #
    # SetRegView 64: the NSIS installer is a 32-bit program and would otherwise
    # write Software\HotDamn under WOW6432Node, where the Capabilities path in
    # RegisteredApplications does not resolve. Set back to 32 afterwards for
    # CPack's own registry writes.
    #
    # The question is only asked on a first install: an update finds the
    # Capabilities key from the previous install and leaves the choice alone.
    #
    # The machine-wide .nfo default is still pointed at the ProgID (it applies
    # where no other viewer is registered), with the previous value kept for
    # the uninstaller to restore.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [[
      SetRegView 64
      Push $1
      ReadRegStr $1 HKLM 'Software\HotDamn\Capabilities' 'ApplicationName'
      WriteRegStr HKLM 'Software\Classes\HotDamn.nfo' '' 'NFO File'
      WriteRegStr HKLM 'Software\Classes\HotDamn.nfo\DefaultIcon' '' '$INSTDIR\HotDamn.exe,0'
      WriteRegStr HKLM 'Software\Classes\HotDamn.nfo\shell\open\command' '' '"$INSTDIR\HotDamn.exe" "%1"'
      WriteRegStr HKLM 'Software\Classes\Applications\HotDamn.exe' 'FriendlyAppName' 'Hot DAMN NFO Viewer'
      WriteRegStr HKLM 'Software\Classes\Applications\HotDamn.exe\shell\open\command' '' '"$INSTDIR\HotDamn.exe" "%1"'
      WriteRegStr HKLM 'Software\Classes\Applications\HotDamn.exe\SupportedTypes' '.nfo' ''
      WriteRegStr HKLM 'Software\Classes\Applications\HotDamn.exe\SupportedTypes' '.diz' ''
      WriteRegStr HKLM 'Software\Classes\.nfo\OpenWithProgids' 'HotDamn.nfo' ''
      WriteRegStr HKLM 'Software\Classes\.diz\OpenWithProgids' 'HotDamn.nfo' ''
      WriteRegStr HKLM 'Software\HotDamn\Capabilities' 'ApplicationName' 'Hot DAMN NFO Viewer'
      WriteRegStr HKLM 'Software\HotDamn\Capabilities' 'ApplicationDescription' 'Viewer for scene-release .nfo files'
      WriteRegStr HKLM 'Software\HotDamn\Capabilities\FileAssociations' '.nfo' 'HotDamn.nfo'
      WriteRegStr HKLM 'Software\HotDamn\Capabilities\FileAssociations' '.diz' 'HotDamn.nfo'
      WriteRegStr HKLM 'Software\RegisteredApplications' 'HotDamn' 'Software\HotDamn\Capabilities'
      StrCmp $1 '' 0 hotdamn_assoc_skip
      MessageBox MB_YESNO|MB_ICONQUESTION 'Make Hot DAMN NFO Viewer the default app for .nfo files?$\r$\n$\r$\nWindows only accepts a default app that you pick yourself, so Setup will open the Default apps settings for Hot DAMN NFO Viewer: choose it for .nfo there.' /SD IDNO IDNO hotdamn_assoc_skip
        Push $0
        ReadRegStr $0 HKLM 'Software\Classes\.nfo' ''
        StrCmp $0 'HotDamn.nfo' hotdamn_assoc_default_done
          WriteRegStr HKLM 'Software\Classes\.nfo' 'HotDamn.backup' '$0'
          WriteRegStr HKLM 'Software\Classes\.nfo' '' 'HotDamn.nfo'
        hotdamn_assoc_default_done:
        Pop $0
        ExecShell 'open' 'ms-settings:defaultapps?registeredAppMachine=HotDamn'
      hotdamn_assoc_skip:
      Pop $1
      SetRegView 32
      System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ]])
    # The SetRegView 32 deletes clean up what 0.1.0 wrote into the 32-bit view.
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [[
      SetRegView 64
      Push $0
      Push $1
      ReadRegStr $0 HKLM 'Software\Classes\.nfo' ''
      StrCmp $0 'HotDamn.nfo' 0 hotdamn_unassoc_done
        ReadRegStr $1 HKLM 'Software\Classes\.nfo' 'HotDamn.backup'
        WriteRegStr HKLM 'Software\Classes\.nfo' '' '$1'
      hotdamn_unassoc_done:
      Pop $1
      Pop $0
      DeleteRegValue HKLM 'Software\Classes\.nfo' 'HotDamn.backup'
      DeleteRegValue HKLM 'Software\Classes\.nfo\OpenWithProgids' 'HotDamn.nfo'
      DeleteRegValue HKLM 'Software\Classes\.diz\OpenWithProgids' 'HotDamn.nfo'
      DeleteRegKey HKLM 'Software\Classes\HotDamn.nfo'
      DeleteRegKey HKLM 'Software\Classes\Applications\HotDamn.exe'
      DeleteRegKey HKLM 'Software\HotDamn'
      DeleteRegValue HKLM 'Software\RegisteredApplications' 'HotDamn'
      SetRegView 32
      DeleteRegKey HKLM 'Software\HotDamn'
      DeleteRegValue HKLM 'Software\RegisteredApplications' 'HotDamn'
      System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ]])

    include(CPack)
endif()
