cmake_minimum_required(VERSION 3.25)
project(IfritView VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Release builds pass the tag's version (CI: v1.2 -> 1.2).
set(IFRIT_VERSION "${PROJECT_VERSION}" CACHE STRING "Version stamped into the binary and packages")

# Windows builds use MSVC with the format libraries from vcpkg (vcpkg.json),
# all linked statically, the C runtime too: one exe, no runtime DLLs.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /Zc:__cplusplus /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  set(IFRIT_WARNINGS /W3)
  if(DEFINED VCPKG_INSTALLED_DIR)
    # vcpkg's pkgconf (a host tool, under the host triplet) over its .pc
    # files, which describe static builds.
    file(GLOB IFRIT_PKGCONF_DIRS "${VCPKG_INSTALLED_DIR}/*/tools/pkgconf")
    find_program(PKG_CONFIG_EXECUTABLE pkgconf HINTS ${IFRIT_PKGCONF_DIRS} REQUIRED)
    set(ENV{PKG_CONFIG_PATH} "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/pkgconfig")
  endif()
else()
  set(IFRIT_WARNINGS -Wall -Wextra)
endif()
if(VCPKG_TARGET_TRIPLET MATCHES "static")
  set(PKG_CONFIG_ARGN --static)  # the private libraries too, for static linking
  add_compile_definitions(JSON_STATIC_BUILD)  # json-glib's .pc doesn't say so
endif()
find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
# The core needs GLib/GIO and gdk-pixbuf; the toolkits come with rapport.
if(WIN32)
  pkg_check_modules(CORE REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 gdk-pixbuf-2.0)
else()
  pkg_check_modules(CORE REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 gio-unix-2.0 gdk-pixbuf-2.0)
endif()

# Everything that isn't UI: decoders, the image model, loader, cache, config,
# folders, video, geocoding.
add_library(ifrit_core STATIC
  src/config.cpp
  src/folder.cpp
  src/image.cpp
  src/decoders/decoder.cpp
  src/decoders/icc.cpp
  src/encoders/encoder.cpp
  src/encoders/reduce.cpp
  src/cache.cpp
  src/geocode.cpp
  src/video.cpp
  src/loader.cpp
  src/scale.cpp
  src/probe.cpp
  src/compare.cpp
  src/platform.cpp
  src/decoders/stb.cpp
  src/decoders/tim.cpp
  src/decoders/ilbm.cpp
  src/decoders/pcx.cpp
  src/decoders/netpbm.cpp
  src/decoders/bcn.cpp
  src/decoders/dds.cpp
  src/decoders/vtf.cpp
  src/decoders/blp.cpp
  src/decoders/tim2.cpp
  src/decoders/gim.cpp
  src/decoders/quake.cpp
  src/decoders/etc.cpp
  src/decoders/pkm.cpp
  src/decoders/pixbuf.cpp
)
target_compile_options(ifrit_core PRIVATE ${IFRIT_WARNINGS})
target_compile_definitions(ifrit_core PUBLIC IFRIT_VERSION="${IFRIT_VERSION}")
target_include_directories(ifrit_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ifrit_core PUBLIC PkgConfig::CORE ${CMAKE_DL_LIBS})
set_source_files_properties(src/decoders/stb.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
if(WIN32)
  target_sources(ifrit_core PRIVATE src/integration_win.cpp)
  target_link_libraries(ifrit_core PUBLIC shell32 advapi32)
else()
  target_sources(ifrit_core PRIVATE src/integration.cpp)
endif()

# The app: native widgets from whichever toolkit is at hand (GTK or Qt on
# Linux, Win32 on Windows), through rapport, linked in statically from the
# third_party/rapport submodule (github.com/TurkeyMan/rapport).
set(IFRIT_RAPPORT_DIR "${CMAKE_SOURCE_DIR}/third_party/rapport" CACHE PATH "rapport source tree")
if(NOT EXISTS "${IFRIT_RAPPORT_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "rapport not found in ${IFRIT_RAPPORT_DIR}: run 'git submodule update --init'")
endif()
# Linux packages ship every toolkit backend as a plugin, loaded at runtime
# for the desktop in use (one whose toolkit isn't installed is skipped), so
# installing pulls in no toolkit; rapport's core goes with them, privately,
# in lib/ifritview. Otherwise the backends are linked in.
option(IFRIT_RAPPORT_PLUGINS "rapport's toolkit backends as plugins in lib/ifritview" OFF)
if(WIN32)
  set(IFRIT_RAPPORT_PLUGINS OFF CACHE BOOL "" FORCE)
endif()
set(IFRIT_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/ifritview")
set(RAPPORT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAPPORT_SHELL OFF CACHE BOOL "" FORCE)  # no file-manager extension (yet)
# One GTK per program when linked in: 3 (the default) or 4. As plugins,
# both (only one is ever loaded). Windows uses neither.
set(IFRIT_GTK 3 CACHE STRING "GTK major version when linked in (3 or 4)")
if(IFRIT_RAPPORT_PLUGINS)
  set(RAPPORT_STATIC OFF CACHE BOOL "" FORCE)
  set(RAPPORT_BACKEND_GTK3 ON CACHE BOOL "" FORCE)
  set(RAPPORT_BACKEND_GTK4 ON CACHE BOOL "" FORCE)
  # In the build tree as installed: the exe, and the libraries beside it in
  # lib/ifritview (relative to the build directory).
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/ifritview)
else()
  set(RAPPORT_STATIC ON CACHE BOOL "" FORCE)
endif()
if(WIN32)
  set(RAPPORT_BACKEND_GTK3 OFF CACHE BOOL "" FORCE)
  set(RAPPORT_BACKEND_GTK4 OFF CACHE BOOL "" FORCE)
elseif(IFRIT_RAPPORT_PLUGINS)
elseif(IFRIT_GTK STREQUAL "4")
  set(RAPPORT_BACKEND_GTK3 OFF CACHE BOOL "" FORCE)
  set(RAPPORT_BACKEND_GTK4 ON CACHE BOOL "" FORCE)
else()
  set(RAPPORT_BACKEND_GTK3 ON CACHE BOOL "" FORCE)
  set(RAPPORT_BACKEND_GTK4 OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(${IFRIT_RAPPORT_DIR} rapport EXCLUDE_FROM_ALL)
add_executable(ifritview
  src/ui/main.cpp
  src/ui/app.cpp
  src/ui/view.cpp
  src/ui/settings.cpp
  src/ui/save.cpp
)
set_target_properties(ifritview PROPERTIES CXX_STANDARD 23)
target_compile_options(ifritview PRIVATE ${IFRIT_WARNINGS})
target_include_directories(ifritview PRIVATE src/ui)
target_link_libraries(ifritview PRIVATE ifrit_core rapport::rapport)
if(WIN32)
  # comctl32 v6, per-monitor DPI v2, UTF-8: an application manifest.
  enable_language(RC)
  target_sources(ifritview PRIVATE data/windows/ifritview.rc)
  # The resource compiler's own inputs, which CMake doesn't see in the .rc.
  set_source_files_properties(data/windows/ifritview.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/data/windows/ifritview.ico;${CMAKE_SOURCE_DIR}/data/windows/ifritview.manifest")
  # A GUI program, so no console window opens beside it; main() stays the
  # entry point. The manifest is the .rc's own.
  set_target_properties(ifritview PROPERTIES WIN32_EXECUTABLE ON)
  if(MSVC)
    target_link_options(ifritview PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
  endif()
endif()
if(IFRIT_RAPPORT_PLUGINS)
  target_compile_definitions(ifritview PRIVATE IFRIT_PLUGIN_DIR="${IFRIT_PLUGIN_DIR}")
  set_target_properties(ifritview PROPERTIES INSTALL_RPATH "$ORIGIN/../${IFRIT_PLUGIN_DIR}")
  install(TARGETS rapport LIBRARY DESTINATION ${IFRIT_PLUGIN_DIR})
  set(IFRIT_PLUGINS "")
  foreach(backend gtk3 gtk4 qt6)
    if(TARGET rapport_${backend})
      set_target_properties(rapport_${backend} PROPERTIES INSTALL_RPATH "$ORIGIN")
      add_dependencies(ifritview rapport_${backend})
      install(TARGETS rapport_${backend} LIBRARY DESTINATION ${IFRIT_PLUGIN_DIR})
      list(APPEND IFRIT_PLUGINS ${backend})
    endif()
  endforeach()
  message(STATUS "rapport plugins: ${IFRIT_PLUGINS}")
else()
  # Qt 6 alongside GTK when it's installed; RAPPORT_BACKEND / --ui choose.
  foreach(backend gtk3 gtk4 qt6 null win32)
    if(TARGET rapport::backend_${backend})
      target_link_libraries(ifritview PRIVATE rapport::backend_${backend})
      target_compile_definitions(ifritview PRIVATE IFRIT_BACKEND_${backend})
    endif()
  endforeach()
endif()

# Optional format backends. Each is on by default and simply left out, with a
# notice, when its library isn't installed.
function(ifrit_backend name define source)
  string(TOUPPER ${name} upper)
  option(IFRIT_WITH_${upper} "Build the ${name} decoder" ON)
  if(NOT IFRIT_WITH_${upper})
    message(STATUS "${name}: disabled")
    return()
  endif()
  pkg_check_modules(${upper} IMPORTED_TARGET ${ARGN})
  if(${upper}_FOUND)
    target_sources(ifrit_core PRIVATE ${source})
    target_compile_definitions(ifrit_core PUBLIC ${define})
    target_link_libraries(ifrit_core PUBLIC PkgConfig::${upper})
    set(IFRIT_HAVE_${upper} TRUE PARENT_SCOPE)
    message(STATUS "${name}: enabled")
  else()
    message(STATUS "${name}: not found (${ARGN}), skipping")
  endif()
endfunction()

ifrit_backend(exif HAVE_EXIF src/decoders/exif.cpp libexif)
ifrit_backend(jpeg HAVE_JPEG src/decoders/jpeg.cpp libjpeg)
ifrit_backend(png  HAVE_PNG  src/decoders/png.cpp  libpng)
ifrit_backend(webp HAVE_WEBP src/decoders/webp.cpp libwebp libwebpdemux)
ifrit_backend(heif HAVE_HEIF src/decoders/heif.cpp libheif)
ifrit_backend(jxl  HAVE_JXL  src/decoders/jxl.cpp  libjxl)
ifrit_backend(tiff HAVE_TIFF src/decoders/tiff.cpp libtiff-4)
ifrit_backend(iptc HAVE_IPTC src/decoders/iptc.cpp libiptcdata)
ifrit_backend(xmp  HAVE_XMP  src/decoders/xmp.cpp  exempi-2.0)

# LibRaw ships a reentrant variant; prefer it, fall back to the plain one.
pkg_check_modules(LIBRAW_R QUIET libraw_r)
if(LIBRAW_R_FOUND)
  ifrit_backend(raw HAVE_RAW src/decoders/raw.cpp libraw_r)
else()
  ifrit_backend(raw HAVE_RAW src/decoders/raw.cpp libraw)
endif()

# Videos: poster frames and playback through GStreamer, which is optional at
# runtime: the build needs its headers, but the viewer dlopens the libraries
# (src/gst_api.cpp) rather than linking them. Without GStreamer installed,
# videos just aren't part of the series. Windows uses its own Media
# Foundation instead (also loaded at runtime: N editions don't have it).
if(WIN32)
  target_sources(ifrit_core PRIVATE src/mf.cpp src/video_mf.cpp src/decoders/video_poster_mf.cpp)
  target_compile_definitions(ifrit_core PUBLIC HAVE_VIDEO)
  target_link_libraries(ifrit_core PUBLIC mfuuid ole32 oleaut32 windowscodecs)
  message(STATUS "video: enabled (Media Foundation)")
else()
pkg_check_modules(VIDEO gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0)
endif()
if(WIN32)
elseif(VIDEO_FOUND)
  target_sources(ifrit_core PRIVATE src/gst_api.cpp src/decoders/video_poster.cpp)
  target_compile_definitions(ifrit_core PUBLIC HAVE_VIDEO)
  target_include_directories(ifrit_core PUBLIC ${VIDEO_INCLUDE_DIRS})
  message(STATUS "video: enabled (GStreamer loaded at runtime)")
else()
  message(STATUS "video: not found (gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 headers), skipping")
endif()

# PDF through poppler, which, like GStreamer, is optional at runtime: built
# against its headers, dlopened by src/decoders/pdf.cpp, never linked. The
# page is drawn with cairo, which is linked.
pkg_check_modules(PDF poppler-glib)
pkg_check_modules(PDF_CAIRO IMPORTED_TARGET cairo)
if(PDF_FOUND AND PDF_CAIRO_FOUND)
  target_sources(ifrit_core PRIVATE src/decoders/pdf.cpp)
  target_compile_definitions(ifrit_core PUBLIC HAVE_PDF)
  target_include_directories(ifrit_core PUBLIC ${PDF_INCLUDE_DIRS})
  target_link_libraries(ifrit_core PUBLIC PkgConfig::PDF_CAIRO)
  message(STATUS "pdf: enabled (poppler loaded at runtime)")
else()
  message(STATUS "pdf: not found (poppler-glib headers), skipping")
endif()

# Reverse geocoding of GPS positions (optional): an HTTPS request through
# rapport (src/http.cpp), with json-glib to read the answer.
pkg_check_modules(GEOCODE IMPORTED_TARGET json-glib-1.0)
if(GEOCODE_FOUND)
  target_sources(ifrit_core PRIVATE src/http.cpp)
  target_link_libraries(ifrit_core PUBLIC rapport::rapport)
  target_compile_definitions(ifrit_core PUBLIC HAVE_GEOCODE)
  target_link_libraries(ifrit_core PUBLIC PkgConfig::GEOCODE)
  message(STATUS "geocode: enabled")
else()
  message(STATUS "geocode: not found (json-glib-1.0), skipping")
endif()

if(IFRIT_HAVE_JXL)
  pkg_check_modules(JXL_THREADS IMPORTED_TARGET libjxl_threads)
  if(JXL_THREADS_FOUND)
    target_compile_definitions(ifrit_core PUBLIC HAVE_JXL_THREADS)
    target_link_libraries(ifrit_core PUBLIC PkgConfig::JXL_THREADS)
  endif()
endif()

# vcpkg's .pc files on MSVC: Unix link names (libm, pthread...) that are in
# the C runtime, and names that miss the file vcpkg installed (-lde265 for
# libde265.lib, -lx265 for x265-static.lib, -lz for zs.lib, -ljxl-static
# for jxl.lib).
if(MSVC)
  set(vcpkg_lib "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
  foreach(pkg CORE EXIF JPEG PNG WEBP HEIF JXL JXL_THREADS TIFF RAW GEOCODE PDF_CAIRO)
    if(NOT TARGET PkgConfig::${pkg})
      continue()
    endif()
    get_target_property(libs PkgConfig::${pkg} INTERFACE_LINK_LIBRARIES)
    list(REMOVE_ITEM libs m pthread dl stdc++ c++ atomic)
    set(fixed "")
    foreach(lib IN LISTS libs)
      if(NOT IS_ABSOLUTE "${lib}" AND NOT lib MATCHES "^-" AND NOT EXISTS "${vcpkg_lib}/${lib}.lib")
        unset(found CACHE)
        string(REGEX REPLACE "[-_]static$" "" base "${lib}")
        find_library(found NAMES lib${lib} ${lib}-static ${lib}_static ${lib}s ${base} lib${base}
          PATHS "${vcpkg_lib}" NO_DEFAULT_PATH)
        if(found)
          set(lib "${found}")
        endif()
        unset(found CACHE)
      endif()
      list(APPEND fixed "${lib}")
    endforeach()
    set_target_properties(PkgConfig::${pkg} PROPERTIES INTERFACE_LINK_LIBRARIES "${fixed}")
  endforeach()
endif()

# Developer tool: generates a folder of test images. Needs the encoders too,
# so it is only built when every library it uses is present.
if(IFRIT_HAVE_WEBP AND IFRIT_HAVE_JXL AND IFRIT_HAVE_HEIF)
  add_executable(mkimages EXCLUDE_FROM_ALL tools/mkimages.cpp)
  target_link_libraries(mkimages PRIVATE PkgConfig::CORE PkgConfig::WEBP PkgConfig::JXL PkgConfig::HEIF)
endif()

if(TARGET ifritview)
  install(TARGETS ifritview RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES data/ifritview.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# The icon at each size, from data/ifritview.png (1024 px).
install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
install(FILES data/ifritview.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The 256 px icon as C bytes, for a bare executable's own desktop entry.
file(READ data/icons/hicolor/256x256/apps/ifritview.png IFRIT_ICON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," IFRIT_ICON_PNG "${IFRIT_ICON_HEX}")
file(READ data/ifritview.desktop IFRIT_DESKTOP_ENTRY)
configure_file(src/embedded.h.in ${CMAKE_BINARY_DIR}/generated/embedded.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS data/icons/hicolor/256x256/apps/ifritview.png
  data/ifritview.desktop)

# Refresh the desktop/MIME/icon caches on `cmake --install` so file managers
# pick the app up immediately (packages do this in their own postinst).
install(CODE "
  foreach(tool update-desktop-database update-mime-database gtk-update-icon-cache)
    find_program(IFRIT_TOOL_\${tool} \${tool})
  endforeach()
  set(data \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR})
  if(IFRIT_TOOL_update-desktop-database)
    execute_process(COMMAND \${IFRIT_TOOL_update-desktop-database} -q \${data}/applications)
  endif()
  if(IFRIT_TOOL_gtk-update-icon-cache AND EXISTS \${data}/icons/hicolor/index.theme)
    execute_process(COMMAND \${IFRIT_TOOL_gtk-update-icon-cache} -q -t -f \${data}/icons/hicolor)
  endif()
")

# Packages: `cpack -G DEB` or `cpack -G RPM` in the build directory. Library
# dependencies are worked out from the binary, so they're the image
# libraries; GStreamer and poppler aren't linked, so they're only recommended,
# as is libcurl (rapport loads it for place-name lookups).
set(CPACK_PACKAGE_NAME "ifritview")
set(CPACK_PACKAGE_VERSION "${IFRIT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Manu Evans <turkeyman@gmail.com>")
set(CPACK_PACKAGE_VENDOR "Manu Evans")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast, no-frills image viewer")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TurkeyMan/ifritview")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
if(IFRIT_RAPPORT_PLUGINS)
  # Not from every file: the toolkit plugins would make each toolkit a
  # dependency. .github/ci/build.sh works them out from the exe and the core.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
else()
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "libcurl4t64 | libcurl4, libpoppler-glib8t64 | libpoppler-glib8, libgstreamer1.0-0, libgstreamer-plugins-base1.0-0, gstreamer1.0-plugins-good, gstreamer1.0-libav, libheif-plugin-libde265, libheif-plugin-dav1d")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
# CPack drops CPACK_RPM_PACKAGE_RECOMMENDS on current rpm (its support probe,
# `rpm --suggests`, fails there), so the line rides in after the URL instead.
set(CPACK_RPM_PACKAGE_URL "https://github.com/TurkeyMan/ifritview\nRecommends: libcurl, poppler-glib, gstreamer1, gstreamer1-plugins-base, gstreamer1-plugins-good")
if(IFRIT_RAPPORT_PLUGINS)
  # The toolkit plugins require nothing (a toolkit missing at runtime is
  # skipped), and the private core is no one else's to depend on.
  set(plugins "${CMAKE_INSTALL_PREFIX}/${IFRIT_PLUGIN_DIR}")
  set(CPACK_RPM_SPEC_MORE_DEFINE "%global __requires_exclude_from ^${plugins}/rapport_
%global __provides_exclude_from ^${plugins}/
%global __requires_exclude ^librapport[.]so")
endif()
# Directories the base system owns; the package must not claim them.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps /usr/share/metainfo)

set(CPACK_GENERATOR "DEB")
include(CPack)
