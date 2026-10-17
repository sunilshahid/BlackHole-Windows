cmake_minimum_required(VERSION 3.20)
project(BlackHole LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/packages/Microsoft.Web.WebView2/build/native/include
)

link_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/packages/Microsoft.Web.WebView2/build/native/x64
)

add_definitions(-DUNICODE -D_UNICODE)

set(SOURCES
    src/main.cpp
    src/AppConfig.cpp
    src/NetworkEngine.cpp
    src/Resource.rc
)

add_executable(BlackHole WIN32 ${SOURCES})

target_link_libraries(BlackHole PRIVATE
    WebView2LoaderStatic.lib
    ws2_32
    iphlpapi
    dwmapi
    ole32
    shell32
    user32
    gdi32
)

# Copy UI directory to build output so BlackHole.exe can find index.html
add_custom_command(TARGET BlackHole POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    $<TARGET_FILE_DIR:BlackHole>/ui
)
