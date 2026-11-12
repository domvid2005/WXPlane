cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, matches vcpkg x64-windows-static
if(CMAKE_HOST_APPLE)   # before project() and the dependencies, so OpenJPEG and libaec are built universal too
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
project(WXPlane LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- X-Plane SDK 4.2.0 (needed for XPLMWeather.h) ---------------------------------------------------
# Download "XPSDK420.zip" from https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and unzip it.
# Pass the unzipped folder: cmake -DXPLANE_SDK=/path/to/SDK ...   (it contains CHeaders/ and Libraries/)
set(XPLANE_SDK "" CACHE PATH "Folder containing CHeaders/ and Libraries/ of the X-Plane SDK (4.2.0 or newer)")
if(NOT XPLANE_SDK OR NOT EXISTS "${XPLANE_SDK}/CHeaders/XPLM/XPLMWeather.h")
  message(FATAL_ERROR "Set -DXPLANE_SDK=<SDK folder>. It must be SDK 4.2.0+ (CHeaders/XPLM/XPLMWeather.h missing).")
endif()

find_package(Threads REQUIRED)

# --- GRIB2 decoders for the turbulence data, built from source and linked statically ----------------------------
# OpenJPEG decodes JPEG2000-packed GRIB2 (NOAA GTGN, template 5.40); libaec decodes CCSDS-packed GRIB2 (ECMWF, 5.42).
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)          # the static CRT setting above also applies to the dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_CODEC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openjpeg GIT_REPOSITORY https://github.com/uclouvain/openjpeg.git GIT_TAG v2.5.3 GIT_SHALLOW TRUE)
FetchContent_Declare(libaec GIT_REPOSITORY https://github.com/MathisRosenhauer/libaec.git GIT_TAG v1.1.3 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openjpeg libaec)
add_library(wx_codecs INTERFACE)
target_link_libraries(wx_codecs INTERFACE openjp2 aec_static)
target_include_directories(wx_codecs INTERFACE
  "${openjpeg_SOURCE_DIR}/src/lib/openjp2" "${openjpeg_BINARY_DIR}/src/lib/openjp2")
target_compile_definitions(wx_codecs INTERFACE OPJ_STATIC)

# --- libcurl ------------------------------------------------------------------------------------------------------------
# Linux: built from source with OpenSSL and zlib linked in statically, and kept private (-Wl,--exclude-libs,ALL below). Linking the
# system libcurl.so.4 broke under Steam's Linux runtime (2026-11-11): it puts its own libcurl.so.4 in the process, built with the
# CURL_OPENSSL_3 symbol versions, and the plugin wanted CURL_OPENSSL_4 ("version `CURL_OPENSSL_4' not found"). A bundled
# libcurl.so.4 would clash with the one already loaded. The certificate bundle is found at run time (http.cpp).
# Windows: vcpkg's static curl (Schannel). macOS: the system libcurl.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(WX_BUNDLE_CURL "Build libcurl, OpenSSL and zlib into the plugin (Linux)" ON)
endif()
if(WX_BUNDLE_CURL)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  find_package(OpenSSL 3 REQUIRED)
  set(ZLIB_USE_STATIC_LIBS ON)
  foreach(opt BUILD_CURL_EXE BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL BUILD_EXAMPLES CURL_USE_LIBPSL CURL_USE_LIBSSH2
              CURL_USE_LIBSSH USE_LIBIDN2 USE_NGHTTP2 USE_NGTCP2 USE_QUICHE USE_MSH3 CURL_BROTLI CURL_ZSTD CURL_USE_GSSAPI ENABLE_ARES
              USE_LIBRTMP CURL_ENABLE_EXPORT_TARGET PICKY_COMPILER CURL_USE_PKGCONFIG CURL_CA_FALLBACK)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(opt CURL_USE_OPENSSL CURL_ZLIB CURL_DISABLE_LDAP CURL_DISABLE_LDAPS CURL_DISABLE_INSTALL CURL_DISABLE_DICT CURL_DISABLE_GOPHER
              CURL_DISABLE_IMAP CURL_DISABLE_MQTT CURL_DISABLE_POP3 CURL_DISABLE_RTSP CURL_DISABLE_SMB CURL_DISABLE_SMTP CURL_DISABLE_TELNET
              CURL_DISABLE_TFTP)
    set(${opt} ON CACHE BOOL "" FORCE)
  endforeach()
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
  FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_14_1/curl-8.14.1.tar.xz
    URL_HASH SHA256=f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd)
  FetchContent_MakeAvailable(curl)
  if(TARGET CURL::libcurl_static)
    set(WX_CURL CURL::libcurl_static)
  else()
    set(WX_CURL CURL::libcurl)
  endif()
else()
  find_package(CURL REQUIRED)
  set(WX_CURL CURL::libcurl)
endif()

set(WX_CORE_SOURCES
  src/json_mini.cpp src/metar.cpp src/sources.cpp src/wxmodel.cpp src/windsaloft.cpp src/config.cpp src/viewer.cpp
  src/inflate.cpp src/png.cpp src/grib2.cpp src/radar.cpp src/windshear.cpp src/gribx.cpp src/turbulence.cpp src/hazards.cpp src/opera.cpp)

# --- The weather window (4.0): Dear ImGui (third_party/imgui, MIT) and the fonts it uses, embedded at build time ------------------
add_library(wx_imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp)
target_include_directories(wx_imgui PUBLIC third_party/imgui)
target_compile_definitions(wx_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_WIN32_FUNCTIONS)
set(WX_FONT_FILES
  ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-Regular.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-SemiBold.ttf
  ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-Bold.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/JetBrainsMono-Regular.ttf
  ${CMAKE_SOURCE_DIR}/third_party/basemap/basemap.bin ${CMAKE_SOURCE_DIR}/third_party/basemap/tzones.bin
  ${CMAKE_SOURCE_DIR}/assets/wxplane_logo.png)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/wx_fonts.c
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/third_party/fonts -DOUT=${CMAKE_BINARY_DIR}/wx_fonts.c -P ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake
  DEPENDS ${WX_FONT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake
  COMMENT "Embedding the weather window fonts")
add_library(wx_ui STATIC src/wxui.cpp ${CMAKE_BINARY_DIR}/wx_fonts.c)
target_link_libraries(wx_ui PUBLIC wx_imgui wx_codecs)

add_library(WXPlane MODULE
  ${WX_CORE_SOURCES} src/http.cpp src/fetcher.cpp src/turbfetch.cpp src/wxgl.cpp src/xpimgui.cpp src/plugin.cpp)

target_include_directories(WXPlane PRIVATE "${XPLANE_SDK}/CHeaders/XPLM")
target_compile_definitions(WXPlane PRIVATE
  XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1 XPLM410=1 XPLM420=1)
target_link_libraries(WXPlane PRIVATE ${WX_CURL} Threads::Threads wx_codecs wx_ui ${CMAKE_DL_LIBS})

# --- Output: <build>/package/WXPlane/<lin_x64|mac_x64|win_x64>/WXPlane.xpl ----------------------
if(WIN32)
  set(WX_PLATFORM_DIR win_x64)
  target_compile_definitions(WXPlane PRIVATE IBM=1 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
  target_link_libraries(WXPlane PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")
elseif(APPLE)
  set(WX_PLATFORM_DIR mac_x64)
  target_compile_definitions(WXPlane PRIVATE APL=1)
  target_link_options(WXPlane PRIVATE -undefined dynamic_lookup)
else()
  set(WX_PLATFORM_DIR lin_x64)
  target_compile_definitions(WXPlane PRIVATE LIN=1)
  # XPLM symbols are resolved by X-Plane when the plugin loads, so undefined references are expected.
  target_link_options(WXPlane PRIVATE -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL)
endif()

set(WX_OUT "${CMAKE_BINARY_DIR}/package/WXPlane/${WX_PLATFORM_DIR}")
# Third-party notices next to the plugin in its platform folder (each platform's artifact carries its own copy, and the merged
# bundle then has one per folder without clashing).
add_custom_command(TARGET WXPlane POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_SOURCE_DIR}/LICENSES.txt" "${WX_OUT}/LICENSES.txt")
# The user manual (install, every tab and setting, troubleshooting, credits) ships next to it.
add_custom_command(TARGET WXPlane POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_SOURCE_DIR}/docs/WXPlane_Manual.txt" "${WX_OUT}/WXPlane_Manual.txt")
# SkunkCrafts Updater: the plugin folder carries skunkcrafts_updater.cfg, which tells the updater app where WXPlane's update depot is
# (the "release" branch of the GitHub repo, made by tools/skunkcrafts_release.sh) and which version this download is.
file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" WX_VERSION_LINE REGEX "#define WX_VERSION ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" WX_VERSION_STR "${WX_VERSION_LINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/version.h")
configure_file("${CMAKE_SOURCE_DIR}/tools/skunkcrafts_updater.cfg.in" "${CMAKE_BINARY_DIR}/package/WXPlane/skunkcrafts_updater.cfg" @ONLY NEWLINE_STYLE UNIX)
set_target_properties(WXPlane PROPERTIES
  PREFIX "" OUTPUT_NAME "WXPlane" SUFFIX ".xpl"
  LIBRARY_OUTPUT_DIRECTORY "${WX_OUT}" RUNTIME_OUTPUT_DIRECTORY "${WX_OUT}")
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${cfg} CFG)
  set_target_properties(WXPlane PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${CFG} "${WX_OUT}" RUNTIME_OUTPUT_DIRECTORY_${CFG} "${WX_OUT}")
endforeach()

# --- Unit tests (no X-Plane needed): ctest -------------------------------------------------------------
option(WX_BUILD_TESTS "Build the parser/model unit tests" ON)
if(WX_BUILD_TESTS)
  enable_testing()
  add_executable(test_wx tests/test_wx.cpp ${WX_CORE_SOURCES})
  target_link_libraries(test_wx PRIVATE wx_codecs)
  target_compile_definitions(test_wx PRIVATE WX_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
  add_test(NAME parser_and_model COMMAND test_wx)
  # the weather window, drawn off-line for every tab (and as PNG pictures where zlib is available)
  add_executable(ui_shot tests/ui_shot.cpp ${WX_CORE_SOURCES})
  target_link_libraries(ui_shot PRIVATE wx_ui)
  target_include_directories(ui_shot PRIVATE src)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(ui_shot PRIVATE WX_UI_PNG=1)
    target_link_libraries(ui_shot PRIVATE ZLIB::ZLIB)
  endif()
  add_test(NAME weather_window COMMAND ui_shot ${CMAKE_BINARY_DIR})
endif()
