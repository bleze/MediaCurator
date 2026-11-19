# donate — the "Support <app>" dialog (PayPal + Lightning), shared by
# MediaCurator and the standalone Hot DAMN NFO Viewer (apps/hotdamn), which
# pulls this directory in from its own CMake project.
# The including project must already have found Qt6 Core/Gui/Widgets.

qt_add_library(donate STATIC
    src/McDonateDialog.cpp
    include/donate/McDonateDialog.h
)

set_target_properties(donate PROPERTIES AUTOMOC ON)

target_include_directories(donate PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(donate PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

qt_add_resources(donate "donate_resources"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        resources/donate/paypal_qr.png
        resources/donate/lightning_qr.png
)

if(MSVC)
    target_compile_options(donate PRIVATE /W4 /WX-)
endif()
