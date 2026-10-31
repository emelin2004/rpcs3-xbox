# UWP shell executable for the Xbox port: a XAML app (Windows.UI.Xaml) with a controller menu.
# LocalState/legacy_ui selects the earlier software-drawn menu (uwp_frontend.cpp) at run time.
file(GLOB UWP_XAML_PAGES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/xaml/*.xaml)

add_executable(rpcs3_uwp WIN32
    uwp_main.cpp
    uwp_input.cpp
    uwp_frontend.cpp
    uwp_xaml.cpp
    ${CMAKE_BINARY_DIR}/uwp_xaml/uwp_xaml_pages.inc
    uwp_firmware.cpp
    uwp_rpcn.cpp
    uwp_telemetry.cpp
    uwp_telemetry_scrub.cpp
    uwp_update.cpp
    uwp_patches.cpp
    uwp_patches_json.cpp
    uwp_textinput.cpp
    uwp_stubs.cpp
    uwp_sysstubs.c
    uwp_lazyimports.c
    uwp_lazyresolve.c
    uwp_heap_guard.cpp
    uwp_d3d12_watch.cpp
    uwp_localized.cpp
    ${CMAKE_BINARY_DIR}/uwp_localized/uwp_localized_strings.inc
    ${CMAKE_SOURCE_DIR}/rpcs3/rpcs3_version.cpp
    # Qt-free input code (upstream builds these in the Qt UI target).
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/blob_detector.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ds3_pad_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ds4_pad_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/dualsense_pad_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/hid_instance.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/hid_pad_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/mm_joystick_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/pad_thread.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/product_info.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ps_move_calibration.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ps_move_config.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ps_move_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ps_move_tracker.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/raw_mouse_config.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/skateboard_pad_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/wiimote_handler.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/xinput_pad_handler.cpp)

target_include_directories(rpcs3_uwp PRIVATE
    ${CMAKE_SOURCE_DIR}/rpcs3
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/uwp_localized
    ${CMAKE_BINARY_DIR}/uwp_xaml)

# The package version (uwp/package/AppxManifest.xml), sent with telemetry reports.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/uwp/package/AppxManifest.xml)
file(READ ${CMAKE_SOURCE_DIR}/uwp/package/AppxManifest.xml UWP_MANIFEST)
string(REGEX MATCH "<Identity [^>]*Version=\"([0-9.]+)\"" _ "${UWP_MANIFEST}")
set(UWP_APP_VERSION "${CMAKE_MATCH_1}")

target_compile_definitions(rpcs3_uwp PRIVATE UNICODE _UNICODE RPCS3_XBOX_VERSION="${UWP_APP_VERSION}")
target_link_libraries(rpcs3_uwp PRIVATE
    rpcs3_emu 3rdparty::wolfssl 3rdparty::libcurl 3rdparty::hidapi 3rdparty::zlib 3rdparty::libusb 3rdparty::stblib 3rdparty::fusion 3rdparty::yaml-cpp
    d3d11 dxgi winmm bcrypt
    # WinRT activation/strings and the UWP *FromApp APIs (import libs from mkimplibs.sh)
    winrt_activation winrt_string uwp_memory_6 uwp_memory_5 uwp_memory_3 uwp_memory_1 uwp_file_fromapp)
# zig ignores -mwindows; UWP apps must be GUI-subsystem executables.
target_link_options(rpcs3_uwp PRIVATE -municode -Wl,--subsystem,windows -Wl,--stack,8388608)

set_target_properties(rpcs3_uwp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/uwp)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The XAML pages and item templates (xaml/*.xaml), embedded as strings for XamlReader.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/uwp_xaml/uwp_xaml_pages.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/uwp_xaml
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_xaml_pages.py ${CMAKE_CURRENT_SOURCE_DIR}/xaml ${CMAKE_BINARY_DIR}/uwp_xaml/uwp_xaml_pages.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_xaml_pages.py ${UWP_XAML_PAGES})

# English overlay strings (home menu, dialogs), taken from the Qt UI's localized_emu.h.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/uwp_localized/uwp_localized_strings.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/uwp_localized
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_localized.py ${CMAKE_SOURCE_DIR}/rpcs3/rpcs3qt/localized_emu.h ${CMAKE_BINARY_DIR}/uwp_localized/uwp_localized_strings.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_localized.py ${CMAKE_SOURCE_DIR}/rpcs3/rpcs3qt/localized_emu.h)

# Diagnostic app: reports which of rpcs3_uwp.exe's imports a device is missing.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/uwp_importcheck/import_table.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/uwp_importcheck
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/uwp/importcheck/gen_table.py $<TARGET_FILE:rpcs3_uwp> ${CMAKE_BINARY_DIR}/uwp_importcheck/import_table.h
    DEPENDS rpcs3_uwp ${CMAKE_SOURCE_DIR}/uwp/importcheck/gen_table.py)
add_executable(rpcs3_uwp_importcheck WIN32
    ${CMAKE_SOURCE_DIR}/uwp/importcheck/import_check.cpp
    ${CMAKE_BINARY_DIR}/uwp_importcheck/import_table.h)
target_include_directories(rpcs3_uwp_importcheck PRIVATE ${CMAKE_BINARY_DIR}/uwp_importcheck)
target_link_libraries(rpcs3_uwp_importcheck PRIVATE winrt_activation winrt_string)
target_link_options(rpcs3_uwp_importcheck PRIVATE -municode -Wl,--subsystem,windows)
set_target_properties(rpcs3_uwp_importcheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
