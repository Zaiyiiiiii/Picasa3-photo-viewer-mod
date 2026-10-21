cmake_minimum_required(VERSION 3.20)
project(picasa_mod CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "PicasaPhotoViewer.exe is 32-bit: configure with an x86 toolchain")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options(/W4 /utf-8)

find_package(libheif CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(GIF REQUIRED)
find_package(FFMPEG REQUIRED)

# libjxl only ships pkg-config files.
add_library(jxl_static INTERFACE)
foreach(lib jxl jxl_threads jxl_cms hwy brotlidec brotlienc brotlicommon lcms2)
  find_library(JXL_DEP_${lib} NAMES ${lib} REQUIRED)
  target_link_libraries(jxl_static INTERFACE ${JXL_DEP_${lib}})
endforeach()
target_compile_definitions(jxl_static INTERFACE JXL_STATIC_DEFINE JXL_THREADS_STATIC_DEFINE JXL_CMS_STATIC_DEFINE)

add_library(codecs STATIC
  src/patch.cpp
  src/codecs/codec.cpp
  src/codecs/color.cpp
  src/codecs/webp.cpp
  src/codecs/heif.cpp
  src/codecs/avif.cpp
  src/codecs/jxl.cpp
  src/codecs/gif.cpp
  src/codecs/wic.cpp
  src/codecs/video.cpp
  src/codecs/audio_out.cpp
)
target_link_libraries(codecs PUBLIC heif avif WebP::webpdemux jxl_static GIF::GIF windowscodecs ole32 gdi32)
target_include_directories(codecs PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(codecs PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(codecs PUBLIC ${FFMPEG_LIBRARIES})

add_library(version SHARED
  src/dllmain.cpp
  src/hooks.cpp
  src/trace.cpp
  src/version_proxy.cpp
  src/filetypes.cpp
  src/decoder.cpp
  src/animator.cpp
  src/picasa_features.cpp
  src/background.cpp
  src/network.cpp
  src/dpi.cpp
  src/uiscale.cpp
  src/settings.cpp
  src/respack.cpp
  src/version.def
)
target_link_libraries(version PRIVATE codecs minhook::minhook user32 gdi32 ws2_32 advapi32 shell32 ole32)
# /debug turns off dropping unreferenced code and folding identical functions; turn them back on.
target_link_options(version PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)

add_executable(codectest tools/codectest.cpp)
target_link_libraries(codectest PRIVATE codecs)
