cmake_minimum_required(VERSION 3.16)
project(IfritView VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0 gdk-pixbuf-2.0)

add_executable(ifritview
  src/main.cpp
  src/app.cpp
  src/viewer.cpp
  src/settings.cpp
  src/config.cpp
  src/folder.cpp
  src/image.cpp
  src/decoders/decoder.cpp
  src/encoders/encoder.cpp
  src/metadata.cpp
  src/integration.cpp
  src/decoders/stb.cpp
  src/decoders/pixbuf.cpp
)
target_compile_options(ifritview PRIVATE -Wall -Wextra)
target_link_libraries(ifritview PRIVATE PkgConfig::GTK)
set_source_files_properties(src/decoders/stb.cpp PROPERTIES COMPILE_OPTIONS "-w")

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
    target_sources(ifritview PRIVATE ${source})
    target_compile_definitions(ifritview PRIVATE ${define})
    target_link_libraries(ifritview PRIVATE PkgConfig::${upper})
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
ifrit_backend(pdf  HAVE_PDF  src/decoders/pdf.cpp  poppler-glib)
ifrit_backend(iptc HAVE_IPTC src/decoders/iptc.cpp libiptcdata)
ifrit_backend(xmp  HAVE_XMP  src/decoders/xmp.cpp  exempi-2.0)

# LibRaw ships a reentrant variant; prefer it, fall back to the plain one.
pkg_check_modules(LIBRAW_R QUIET libraw_r)
if(LIBRAW_R_FOUND)
  ifrit_backend(raw HAVE_RAW src/decoders/raw.cpp libraw_r)
else()
  ifrit_backend(raw HAVE_RAW src/decoders/raw.cpp libraw)
endif()

if(IFRIT_HAVE_JXL)
  pkg_check_modules(JXL_THREADS IMPORTED_TARGET libjxl_threads)
  if(JXL_THREADS_FOUND)
    target_compile_definitions(ifritview PRIVATE HAVE_JXL_THREADS)
    target_link_libraries(ifritview PRIVATE PkgConfig::JXL_THREADS)
  endif()
endif()

# Developer tool: generates a folder of test images. Needs the encoders too,
# so it is only built when every library it uses is present.
if(IFRIT_HAVE_WEBP AND IFRIT_HAVE_JXL AND IFRIT_HAVE_HEIF)
  add_executable(mkimages EXCLUDE_FROM_ALL tools/mkimages.cpp)
  target_link_libraries(mkimages PRIVATE PkgConfig::GTK PkgConfig::WEBP PkgConfig::JXL PkgConfig::HEIF)
endif()

include(GNUInstallDirs)
install(TARGETS ifritview RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/ifritview.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/ifritview.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/ifritview.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
target_compile_definitions(ifritview PRIVATE IFRIT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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

# `cpack -G DEB` in the build directory produces an installable package with
# library dependencies worked out from the binary.
set(CPACK_PACKAGE_NAME "ifritview")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Manu Evans <turkeyman@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast, no-frills image viewer")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libheif-plugin-libde265, libheif-plugin-dav1d")
set(CPACK_GENERATOR "DEB;TGZ")
include(CPack)
