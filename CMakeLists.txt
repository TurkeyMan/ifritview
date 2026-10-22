cmake_minimum_required(VERSION 3.16)
project(IfritView VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Release builds pass the tag's version (CI: v1.2 -> 1.2).
set(IFRIT_VERSION "${PROJECT_VERSION}" CACHE STRING "Version stamped into the binary and packages")

find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
# The core needs GLib/GIO and gdk-pixbuf; GTK only for the GTK front end.
if(WIN32)
  pkg_check_modules(CORE REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 gdk-pixbuf-2.0)
else()
  pkg_check_modules(CORE REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 gio-unix-2.0 gdk-pixbuf-2.0)
endif()
if(NOT WIN32)
  pkg_check_modules(GTK IMPORTED_TARGET gtk+-3.0)
endif()

# Everything that isn't UI: decoders, the image model, loader, cache, config,
# folders, video, geocoding. Both front ends link it.
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
  src/platform.cpp
  src/decoders/stb.cpp
  src/decoders/tim.cpp
  src/decoders/ilbm.cpp
  src/decoders/pcx.cpp
  src/decoders/pixbuf.cpp
)
target_compile_options(ifrit_core PRIVATE -Wall -Wextra)
target_compile_definitions(ifrit_core PUBLIC IFRIT_VERSION="${IFRIT_VERSION}")
target_include_directories(ifrit_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ifrit_core PUBLIC PkgConfig::CORE ${CMAKE_DL_LIBS})
set_source_files_properties(src/decoders/stb.cpp PROPERTIES COMPILE_OPTIONS "-w")
if(WIN32)
  target_sources(ifrit_core PRIVATE src/integration_win.cpp)
  target_link_libraries(ifrit_core PUBLIC shell32 advapi32)
else()
  target_sources(ifrit_core PRIVATE src/integration.cpp)
endif()

# The GTK 3 front end (the shipping one on Linux, until the rapport one has caught up).
if(GTK_FOUND)
  add_executable(ifritview
    src/main.cpp
    src/app.cpp
    src/viewer.cpp
    src/settings.cpp
    src/metadata.cpp
    src/icons.cpp
  )
  target_compile_options(ifritview PRIVATE -Wall -Wextra)
  target_link_libraries(ifritview PRIVATE ifrit_core PkgConfig::GTK)
endif()

# The rapport front end: native widgets from whichever toolkit is at hand (GTK 3
# here, Win32 on Windows), linked in statically from the third_party/rapport submodule
# (github.com/TurkeyMan/rapport).
set(IFRIT_RAPPORT_DIR "${CMAKE_SOURCE_DIR}/third_party/rapport" CACHE PATH "rapport source tree")
option(IFRIT_UI_RAPPORT "Build the rapport front end (ifritview-rapport)" ON)
if(IFRIT_UI_RAPPORT AND EXISTS "${IFRIT_RAPPORT_DIR}/CMakeLists.txt")
  set(RAPPORT_STATIC ON CACHE BOOL "" FORCE)
  set(RAPPORT_EXAMPLES OFF CACHE BOOL "" FORCE)
  # One GTK per program: 3 (the default) or 4. Windows uses neither.
  set(IFRIT_GTK 3 CACHE STRING "GTK major version for the rapport front end (3 or 4)")
  if(WIN32)
    set(RAPPORT_BACKEND_GTK3 OFF CACHE BOOL "" FORCE)
    set(RAPPORT_BACKEND_GTK4 OFF CACHE BOOL "" FORCE)
  elseif(IFRIT_GTK STREQUAL "4")
    set(RAPPORT_BACKEND_GTK3 OFF CACHE BOOL "" FORCE)
    set(RAPPORT_BACKEND_GTK4 ON CACHE BOOL "" FORCE)
  else()
    set(RAPPORT_BACKEND_GTK3 ON CACHE BOOL "" FORCE)
    set(RAPPORT_BACKEND_GTK4 OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(${IFRIT_RAPPORT_DIR} rapport EXCLUDE_FROM_ALL)
  add_executable(ifritview-rapport
    src/ui/main.cpp
    src/ui/app.cpp
    src/ui/view.cpp
    src/ui/settings.cpp
    src/ui/save.cpp
  )
  set_target_properties(ifritview-rapport PROPERTIES CXX_STANDARD 23)
  target_compile_options(ifritview-rapport PRIVATE -Wall -Wextra)
  target_include_directories(ifritview-rapport PRIVATE src/ui)
  target_link_libraries(ifritview-rapport PRIVATE ifrit_core rapport::rapport)
  if(WIN32)
    # comctl32 v6, per-monitor DPI v2, UTF-8: an application manifest.
    enable_language(RC)
    target_sources(ifritview-rapport PRIVATE data/windows/ifritview.rc)
    set_target_properties(ifritview-rapport PROPERTIES OUTPUT_NAME ifritview)
  endif()
  # Qt 6 alongside GTK when it's installed; RAPPORT_BACKEND / --ui choose.
  foreach(backend gtk3 gtk4 qt6 null win32)
    if(TARGET rapport::backend_${backend})
      target_link_libraries(ifritview-rapport PRIVATE rapport::backend_${backend})
      target_compile_definitions(ifritview-rapport PRIVATE IFRIT_BACKEND_${backend})
    endif()
  endforeach()
  message(STATUS "rapport front end: enabled (${IFRIT_RAPPORT_DIR})")
else()
  message(STATUS "rapport front end: off (no ${IFRIT_RAPPORT_DIR})")
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
# against its headers, dlopened by src/decoders/pdf.cpp, never linked.
pkg_check_modules(PDF poppler-glib)
if(PDF_FOUND)
  target_sources(ifrit_core PRIVATE src/decoders/pdf.cpp)
  target_compile_definitions(ifrit_core PUBLIC HAVE_PDF)
  target_include_directories(ifrit_core PUBLIC ${PDF_INCLUDE_DIRS})
  message(STATUS "pdf: enabled (poppler loaded at runtime)")
else()
  message(STATUS "pdf: not found (poppler-glib headers), skipping")
endif()

# Reverse geocoding of GPS positions (optional): a plain HTTPS request over
# GIO (src/http.cpp), with json-glib to read the answer.
pkg_check_modules(GEOCODE IMPORTED_TARGET json-glib-1.0)
if(GEOCODE_FOUND)
  target_sources(ifrit_core PRIVATE src/http.cpp)
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
install(FILES data/ifritview.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/ifritview.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
file(READ data/ifritview.svg IFRIT_ICON_SVG)
file(READ data/ifritview.desktop IFRIT_DESKTOP_ENTRY)
configure_file(src/embedded.h.in ${CMAKE_BINARY_DIR}/generated/embedded.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS data/ifritview.svg data/ifritview.desktop)

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
# as is glib-networking (TLS for place-name lookups).
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
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "glib-networking, libpoppler-glib8t64 | libpoppler-glib8, libgstreamer1.0-0, libgstreamer-plugins-base1.0-0, gstreamer1.0-plugins-good, gstreamer1.0-libav, libheif-plugin-libde265, libheif-plugin-dav1d")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
# CPack drops CPACK_RPM_PACKAGE_RECOMMENDS on current rpm (its support probe,
# `rpm --suggests`, fails there), so the line rides in after the URL instead.
set(CPACK_RPM_PACKAGE_URL "https://github.com/TurkeyMan/ifritview\nRecommends: glib-networking, poppler-glib, gstreamer1, gstreamer1-plugins-base, gstreamer1-plugins-good")
# Directories the base system owns; the package must not claim them.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps /usr/share/metainfo)

set(CPACK_GENERATOR "DEB")
include(CPack)
