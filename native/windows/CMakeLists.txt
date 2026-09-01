cmake_minimum_required(VERSION 3.16)
project(DesktopWebView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(WEBVIEW2_VERSION "1.0.2903.40")
FetchContent_Declare(
  microsoft_webview2
  URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(microsoft_webview2)

set(WEBVIEW2_ROOT "${microsoft_webview2_SOURCE_DIR}")
set(WEBVIEW2_INCLUDE "${WEBVIEW2_ROOT}/build/native/include")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WEBVIEW2_LIB_DIR "${WEBVIEW2_ROOT}/build/native/x64")
else()
  set(WEBVIEW2_LIB_DIR "${WEBVIEW2_ROOT}/build/native/x86")
endif()

add_executable(DesktopWebView WIN32
  src/main.cpp
  src/config.cpp
  src/json_util.cpp
  src/rpc_server.cpp
  src/web_window.cpp
  src/host_controller.cpp
)

target_include_directories(DesktopWebView PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${WEBVIEW2_INCLUDE}
)

target_compile_definitions(DesktopWebView PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
)

target_link_libraries(DesktopWebView PRIVATE
  nlohmann_json::nlohmann_json
  "${WEBVIEW2_LIB_DIR}/WebView2LoaderStatic.lib"
  ws2_32
  ole32
  oleaut32
  uuid
  shell32
  user32
  gdi32
  gdiplus
  comctl32
  advapi32
  shlwapi
  version
)

install(TARGETS DesktopWebView RUNTIME DESTINATION bin)
