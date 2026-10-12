cmake_minimum_required(VERSION 3.24)
project(GalaxyQuest C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DECOMP ${CMAKE_SOURCE_DIR}/decomp)

if(ANDROID)
  set(PORT_PLATFORM android)
else()
  set(PORT_PLATFORM desktop)
  # libgame.so is a shared library on Linux too (platform/linux/launcher.c).
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ---------------------------------------------------------------------------
# Decompiled game sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE GAME_SRCS CONFIGURE_DEPENDS ${DECOMP}/src/Game/*.cpp ${DECOMP}/src/Game/*.c)
file(GLOB_RECURSE JSYSTEM_SRCS CONFIGURE_DEPENDS ${DECOMP}/src/JSystem/*.cpp)
file(GLOB_RECURSE NW4R_SRCS CONFIGURE_DEPENDS ${DECOMP}/src/nw4r/*.cpp)

# SDK libraries that are pure software and are compiled from the decomp as-is.
set(SDK_MODULES gx gd mem arc tpl aralt)
set(SDK_SRCS)
foreach(m ${SDK_MODULES})
  file(GLOB_RECURSE _s CONFIGURE_DEPENDS ${DECOMP}/src/RVL_SDK/${m}/*.c)
  list(APPEND SDK_SRCS ${_s})
endforeach()

list(FILTER SDK_SRCS EXCLUDE REGEX "/gx/GXFifo\\.c$|/gx/GXPerf\\.c$")
# OS pieces that are pure software on top of the thread API.
list(APPEND SDK_SRCS ${DECOMP}/src/RVL_SDK/os/OSMutex.c ${DECOMP}/src/RVL_SDK/os/OSMessage.c)
# THP movie audio is plain C (the video decoder is replaced by platform/src/sdk/thp_video.cpp).
list(APPEND SDK_SRCS ${DECOMP}/src/RVL_SDK/thp/THPAudio.c)

set(DECOMP_INCLUDES
  ${CMAKE_SOURCE_DIR}/platform/include
  ${DECOMP}/include
  ${DECOMP}/libs/JSystem/include
  ${DECOMP}/libs/RVL_SDK/include
  ${DECOMP}/libs/nw4r/include
  ${DECOMP}/libs/RVLFaceLib/include
)

set(PORT_DEFS TARGET_PC=1 NDEBUG=1 VERSION=0)

# Paths in the binaries (__FILE__, debug info) relative to the repository
# and the NDK, not the machine that built them.
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}/= -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
if(ANDROID)
  add_compile_options(-ffile-prefix-map=${ANDROID_NDK}=ndk)
endif()

set(PORT_COMMON_FLAGS
  -fshort-wchar
  -fsigned-char
  -fno-strict-aliasing
  -fwrapv
  -ffp-contract=on
  -Wno-register
  -Wno-multichar
  -Wno-invalid-offsetof
  -Wno-c++11-narrowing
  -Wno-writable-strings
  -Wno-null-conversion
  -Wno-deprecated-declarations
  -Wno-int-to-pointer-cast
  -Wno-pointer-to-int-cast
  -Wno-invalid-source-encoding
  -Wno-unused-value
  -Wno-return-type
  -Wno-switch
  -Wno-parentheses
  -Wno-dangling-else
  -Wno-tautological-compare
  -Wno-tautological-undefined-compare
  -Wno-undefined-bool-conversion
  -Wno-constant-conversion
  -Wno-shift-negative-value
  -Wno-implicit-function-declaration
  -Wno-int-conversion
  -Wno-incompatible-pointer-types
  -Wno-incompatible-function-pointer-types
  -Wno-pointer-sign
  -Wno-unknown-pragmas
  -Wno-ignored-attributes
  -Wno-microsoft-template
  -fms-extensions
  -Wno-void-pointer-to-int-cast
  -Wno-microsoft-cast
  -Wno-microsoft-anon-tag
  -Wno-microsoft-include
  -Wno-language-extension-token
  -ferror-limit=0
  -include ${CMAKE_SOURCE_DIR}/platform/include/port/compat.h
)
set(PORT_CXX_FLAGS -fno-exceptions -fno-rtti)
set(PORT_C_FLAGS)

add_library(decomp OBJECT ${GAME_SRCS} ${JSYSTEM_SRCS} ${NW4R_SRCS} ${SDK_SRCS})
target_include_directories(decomp PRIVATE ${DECOMP_INCLUDES})
target_compile_definitions(decomp PRIVATE ${PORT_DEFS})
target_compile_options(decomp PRIVATE ${PORT_COMMON_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:${PORT_CXX_FLAGS}> $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)

# ---------------------------------------------------------------------------
# Port platform layer + final game library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PLATFORM_SRCS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/platform/src/*.c
  ${CMAKE_SOURCE_DIR}/platform/src/*.cpp)

add_library(game SHARED $<TARGET_OBJECTS:decomp> ${PLATFORM_SRCS})
target_include_directories(game PRIVATE ${DECOMP_INCLUDES} ${CMAKE_SOURCE_DIR}/tools/glsl)
target_compile_definitions(game PRIVATE ${PORT_DEFS} PORT_PLATFORM_CODE=1)
target_compile_options(game PRIVATE ${PORT_COMMON_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:${PORT_CXX_FLAGS}> $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)
target_link_options(game PRIVATE -Wl,--no-undefined -Wl,--error-limit=0)
if(ANDROID)
  # -Bsymbolic: game code must bind to the port's operator new/delete, not the
  # system C++ runtime's.
  target_link_options(game PRIVATE -Wl,-Bsymbolic -Wl,--gc-sections)
  target_link_libraries(game PRIVATE log android)
endif()

if(ANDROID)
  # The Khronos OpenXR loader (tools/fetch_openxr.sh puts it there).
  set(OPENXR_DIR ${CMAKE_SOURCE_DIR}/third_party/openxr/prefab/modules)
  set(GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)

  add_library(openxr_loader SHARED IMPORTED)
  set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION ${OPENXR_DIR}/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so)

  target_include_directories(game PRIVATE ${OPENXR_DIR}/headers/include ${GLUE_DIR})
  target_link_libraries(game PRIVATE openxr_loader EGL GLESv3 aaudio)

  # Launcher: reserves the 32-bit game window and loads libgame.so into it.
  add_library(main SHARED platform/android/launcher.c ${GLUE_DIR}/android_native_app_glue.c)
  target_include_directories(main PRIVATE ${GLUE_DIR})
  target_link_libraries(main PRIVATE android log dl)
  target_link_options(main PRIVATE -u ANativeActivity_onCreate)

  # Preloaded into the app process by wrap.sh: reserves the game window before
  # ART lays out its heap.
  add_library(reserve SHARED platform/android/reserve.c)
  target_link_libraries(reserve PRIVATE log)

  # Bring-up tool: boots libgame.so from an adb shell without VR.
  add_executable(petari_headless platform/android/headless.c)
  target_link_libraries(petari_headless PRIVATE log dl m)
  target_include_directories(petari_headless PRIVATE platform/include)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Linux arm64 (the Steam Frame): GLES through EGL, OpenXR through the
  # SteamVR runtime's own loader, sound through SDL2.  Build with clang and
  # lld (see build_linux.sh).
  set(OPENXR_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/openxr/prefab/modules/headers/include)
  set(OPENXR_LOADER /opt/steamvr/bin/linuxarm64/libopenxr_loader.so CACHE FILEPATH "OpenXR loader (libopenxr_loader.so)")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)

  add_library(openxr_loader SHARED IMPORTED)
  set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION ${OPENXR_LOADER} IMPORTED_NO_SONAME ON)

  target_include_directories(game PRIVATE ${OPENXR_INCLUDE} ${SDL2_INCLUDE_DIRS})
  target_link_libraries(game PRIVATE openxr_loader EGL GLESv2 ${SDL2_LIBRARIES} pthread dl m)
  # Loaded at the 0x98000000 slot of the game's address window (its image
  # base), its own references bound to itself (the port's operator new and
  # delete), as on Android.  The OpenXR loader is SteamVR's (it has no soname,
  # so it is found by file name: beside libgame.so, else in SteamVR).
  get_filename_component(OPENXR_LOADER_DIR ${OPENXR_LOADER} DIRECTORY)
  target_link_options(game PRIVATE -fuse-ld=lld -Wl,--image-base=0x98000000 -Wl,-Bsymbolic -Wl,--gc-sections
    "-Wl,-rpath,\$ORIGIN:${OPENXR_LOADER_DIR}")

  add_executable(galaxyquest platform/linux/launcher.c)
  target_link_libraries(galaxyquest PRIVATE dl)
  add_dependencies(galaxyquest game)

  # The headless test runner (tools/run_headless.sh on Android), run directly.
  add_executable(petari_headless platform/android/headless.c)
  target_link_libraries(petari_headless PRIVATE dl m)
  target_include_directories(petari_headless PRIVATE platform/include)
endif()

# ---------------------------------------------------------------------------
# Shader toolchain for the Vulkan backend (Linux only)
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  # libgq_glsl.so: GLSL to SPIR-V through glslang's C API, linked statically.
  # A separate library with ordinary compile flags, so the game library keeps
  # its own (no exceptions, no RTTI, -Bsymbolic, fixed image base).  The game
  # dlopens it from its own directory (platform/src/gx/vk_spirv.cpp).
  set(GQ_GLSLANG_LIB_DIR /usr/lib CACHE PATH "Directory with the static glslang libraries")
  set(GQ_GLSLANG_LIBS glslang-default-resource-limits glslang MachineIndependent GenericCodeGen OSDependent SPIRV)
  set(GQ_GLSLANG_ARCHIVES)
  set(GQ_GLSLANG_FOUND TRUE)
  foreach(l ${GQ_GLSLANG_LIBS})
    if(EXISTS ${GQ_GLSLANG_LIB_DIR}/lib${l}.a)
      list(APPEND GQ_GLSLANG_ARCHIVES ${GQ_GLSLANG_LIB_DIR}/lib${l}.a)
    else()
      set(GQ_GLSLANG_FOUND FALSE)
    endif()
  endforeach()
  # glslang's SPIRV library wants SPIRV-Tools (optimiser, validator), which is
  # only installed as shared libraries.  gq_glsl never uses either, so by
  # default stand-in functions replace SpvTools.cpp and the library has no
  # SPIRV-Tools dependency.  OFF links the shared SPIRV-Tools libraries instead.
  option(GQ_GLSL_STUB_SPVTOOLS "Build libgq_glsl.so without a SPIRV-Tools dependency" ON)

  if(GQ_GLSLANG_FOUND)
    add_library(gq_glsl SHARED tools/glsl/gq_glsl.c)
    set_target_properties(gq_glsl PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden LINKER_LANGUAGE CXX)
    target_include_directories(gq_glsl PRIVATE tools/glsl)
    if(GQ_GLSL_STUB_SPVTOOLS)
      target_sources(gq_glsl PRIVATE tools/glsl/gq_glsl_nospvtools.cpp)
      set_source_files_properties(tools/glsl/gq_glsl_nospvtools.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
    endif()
    target_link_libraries(gq_glsl PRIVATE -Wl,--start-group ${GQ_GLSLANG_ARCHIVES} -Wl,--end-group pthread)
    if(NOT GQ_GLSL_STUB_SPVTOOLS)
      target_link_libraries(gq_glsl PRIVATE SPIRV-Tools-opt SPIRV-Tools)
      set_target_properties(gq_glsl PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
    endif()
    target_link_options(gq_glsl PRIVATE -Wl,--exclude-libs,ALL -Wl,--no-undefined)
    add_dependencies(game gq_glsl)
  else()
    message(STATUS "static glslang libraries not found in ${GQ_GLSLANG_LIB_DIR}: libgq_glsl.so (Vulkan backend) is not built")
  endif()

  # gq_compile_glsl(<target> OUTDIR <dir> SOURCES <file.vert|file.frag|...> ...)
  # compiles fixed shaders with glslc into <dir>/<file>_<stage>_spv.h, each
  # defining `static const uint32_t <file>_<stage>_spv[]` (glslc -mfmt=c).
  find_program(GQ_GLSLC glslc)
  function(gq_compile_glsl target)
    cmake_parse_arguments(A "" "OUTDIR" "SOURCES" ${ARGN})
    if(NOT GQ_GLSLC)
      message(FATAL_ERROR "glslc not found (gq_compile_glsl)")
    endif()
    file(MAKE_DIRECTORY ${A_OUTDIR})
    set(outputs)
    foreach(src ${A_SOURCES})
      get_filename_component(abs ${src} ABSOLUTE)
      get_filename_component(name ${src} NAME)
      string(MAKE_C_IDENTIFIER ${name} id)
      set(inc ${A_OUTDIR}/${id}.inc)
      set(hdr ${A_OUTDIR}/${id}_spv.h)
      file(WRITE ${hdr}.tmp "#include <stdint.h>\nstatic const uint32_t ${id}_spv[] =\n#include \"${id}.inc\"\n;\n")
      execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${hdr}.tmp ${hdr})
      add_custom_command(OUTPUT ${inc}
        COMMAND ${GQ_GLSLC} --target-env=vulkan1.2 -O -mfmt=c -o ${inc} ${abs}
        DEPENDS ${abs} VERBATIM COMMENT "glslc ${name}")
      list(APPEND outputs ${inc})
    endforeach()
    add_custom_target(${target} DEPENDS ${outputs})
  endfunction()

  # Checks and the converter for the generated shaders (tools/glsl/spirv_check.cpp):
  # GLES text hashes, spirv.bin from a shaders.bin, dumps for spirv-val.
  if(GQ_GLSLANG_FOUND AND GQ_GLSLC)
    gq_compile_glsl(gq_fixed_shaders OUTDIR ${CMAKE_BINARY_DIR}/shaders SOURCES tools/glsl/test/smoke.vert)
    add_executable(spirv_check tools/glsl/spirv_check.cpp platform/src/gx/gl_shadergen.cpp platform/src/gx/vk_spirv.cpp)
    target_include_directories(spirv_check PRIVATE platform/src/gx platform/include tools/glsl ${CMAKE_BINARY_DIR}/shaders)
    target_link_libraries(spirv_check PRIVATE dl pthread)
    add_dependencies(spirv_check gq_glsl gq_fixed_shaders)
    # Hashes of the generated GLES text, to compare two generator versions.
    add_executable(gles_hashes tools/glsl/gles_hashes.cpp platform/src/gx/gl_shadergen.cpp)
    target_include_directories(gles_hashes PRIVATE platform/src/gx platform/include)
  endif()
endif()
