cmake_minimum_required(VERSION 3.20)
project(dgpu_tray_switch VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()
if(MINGW)
    # Static runtime so the exe runs standalone (e.g. from the logon task).
    add_link_options(-static -static-libgcc -static-libstdc++ -municode)
endif()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

# MinGW embeds the manifest through the resource script; MSVC-ABI linkers
# merge .manifest sources themselves and would report a duplicate resource.
set(APP_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest)
if(MINGW)
    set(APP_MANIFEST_RESOURCE "1 RT_MANIFEST \"${APP_MANIFEST}\"")
    set(APP_MANIFEST_SOURCE "")
else()
    set(APP_MANIFEST_RESOURCE "")
    set(APP_MANIFEST_SOURCE ${APP_MANIFEST})
endif()

# The app icon is rendered at build time by the same code that draws the tray icons.
add_executable(icongen tools/icongen.cpp src/icons.cpp)
target_include_directories(icongen PRIVATE src)
target_link_libraries(icongen PRIVATE user32 gdi32)
set(APP_ICON ${CMAKE_CURRENT_BINARY_DIR}/app.ico)
add_custom_command(OUTPUT ${APP_ICON} COMMAND icongen ${APP_ICON} DEPENDS icongen VERBATIM)

configure_file(res/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)

file(GLOB APP_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(dgpu_tray_switch WIN32 ${APP_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/app.rc ${APP_MANIFEST_SOURCE})
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/app.rc PROPERTIES OBJECT_DEPENDS "${APP_MANIFEST};${APP_ICON}")
if(NOT MINGW)
    # Our manifest carries requestedExecutionLevel; drop the linker's default one.
    target_link_options(dgpu_tray_switch PRIVATE "LINKER:/MANIFESTUAC:NO")
endif()
target_link_libraries(dgpu_tray_switch PRIVATE user32 shell32 setupapi cfgmgr32 advapi32 gdi32)
