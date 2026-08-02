cmake_minimum_required(VERSION 3.16)
project(DesktopWebView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  gtk4
  webkitgtk-6.0
  libnotify
  json-glib-1.0
)

add_executable(DesktopWebView
  src/main.cpp
  src/config.cpp
  src/json_util.cpp
  src/rpc_server.cpp
  src/web_window.cpp
  src/host_controller.cpp
)

target_include_directories(DesktopWebView PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(DesktopWebView PRIVATE PkgConfig::DEPS)

install(TARGETS DesktopWebView RUNTIME DESTINATION bin)
