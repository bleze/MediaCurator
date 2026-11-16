# nfoview — NFO decoding (CP437 & friends), BBCode conversion and the viewer
# widget. Shared by MediaCurator and the standalone Hot DAMN NFO Viewer
# (apps/hotdamn), which pulls this directory in from its own CMake project.
# The including project must already have found Qt6 Core/Gui/Widgets/Network.

qt_add_library(nfoview STATIC
    src/NfoText.cpp
    src/McNfoView.cpp
    include/nfoview/NfoText.h
    include/nfoview/McNfoView.h
)

set_target_properties(nfoview PROPERTIES AUTOMOC ON)

target_include_directories(nfoview PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(nfoview PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

if(MSVC)
    target_compile_options(nfoview PRIVATE /W4 /WX-)
endif()
