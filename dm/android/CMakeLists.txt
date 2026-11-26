# Android build of the host (arm64-v8a): the shared host sources (dm/host, the C parts of dm/gfx) with the Darwin stand-ins of compat/, the
# Android front end and graphics backend. Two products from one set of objects, and the app's loader:
#   libdmhost.so         the GENERIC game host in the app: one host runs every game. The game profile is read at run time from the game's
#                        folder (game_profile_rt.c: <dir>/profile.txt), not compiled in, so the phone can port a game on the device and run
#                        the resulting pack with this one host (scripts/build_android_from_nsp.py's per-game libdmhost_<GAME>.so is gone).
#   dmhost               a command-line host for adb shell (headless: no window, silent audio unless DM_AUDIO=1), like the Mac's dmhost
#   libnxloader.so       the NativeActivity's library: starts the host for the game the launcher picked (loader.c)
# Configure with the NDK's toolchain file. GAME is accepted for the shader/profile tools but no longer selects a compiled-in profile:
#   cmake -G Ninja -S dm/android -B <build dir> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
#         -DANDROID_PLATFORM=android-29 [-DGAME=<base game title ID>]
cmake_minimum_required(VERSION 3.22)
project(dmhost_android C ASM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN}/asm)

# The profile header is now a thin shim to the runtime profile (game_profile_rt.h): the host reads the
# game's constants from <game dir>/profile.txt at start-up, not from a compile-time generated header. The
# generated file keeps the name game_profile.h so dm.h and front_android.c include it unchanged.
set(GAME_H ${GEN}/game_profile.h)
add_custom_target(game_profile ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND ${CMAKE_COMMAND} -E touch ${GAME_H}
  BYPRODUCTS ${GAME_H} COMMENT "runtime game profile shim")
if(NOT EXISTS ${GAME_H})
  file(WRITE ${GAME_H} "// Generated: the generic host reads its game profile at run time (game_profile_rt.c).\n#pragma once\n#include \"game_profile_rt.h\"\n")
endif()

# Assembly: the Mach-O sources converted to ELF (tools/asm_elf.py)
set(ASM_ELF)
foreach(s ${DM}/host/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  add_custom_command(OUTPUT ${GEN}/asm/${n}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py ${s} ${GEN}/asm/${n}
    DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py)
  list(APPEND ASM_ELF ${GEN}/asm/${n})
endforeach()

# Host sources: every dm/host/*.c, as the Mac Makefile's wildcard, less the ones Android replaces
file(GLOB HOST_C ${DM}/host/*.c)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_movie_avc.c)   # VideoToolbox: media_android.c (MediaCodec)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_cheats.c)     # test-only (t_cheats.c builds it with loader/heap/key stubs); its DM_CHEAT_* macros and dm_guest_heap_*/dm_module_dir are not defined for a host build, and nothing on Android calls its dm_cheats_* interface
# Graphics: the NVN interface layer and texture code (the Metal replay layer, the Vulkan and GL layers are Objective-C on Metal: backend_*.c stand in)
file(GLOB GFX_C ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
list(REMOVE_ITEM GFX_C ${DM}/gfx/nvn/nvn_capture.c)   # Metal frame capture (nvn_mtl.m): uses Apple libcompression (LZFSE); the Android replay is Vulkan (gfx_vk.c), which calls none of its functions
file(GLOB_RECURSE VPX_C ${DM}/third_party/libvpx/*.c)

# The NVN replay: gfx_vk.c (Vulkan) by default; -DDM_GFX=null builds gfx_null.c (no GPU: submissions completed on the CPU, nothing drawn)
set(DM_GFX vk CACHE STRING "NVN replay backend: vk or null")
set(ANDROID_C
  ${CMAKE_CURRENT_SOURCE_DIR}/plat_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_aaudio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/haptics_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/apple_stubs.c
  ${CMAKE_CURRENT_SOURCE_DIR}/gfx_${DM_GFX}.c)

# char is signed, as on Apple arm64 (the host and its third-party code were written there); AArch64 Linux / Android makes plain char unsigned. bcdec read BC4 / BC5 SNORM
# endpoints through (char): negative ones came out as 128..255 and every signed normal map decoded to noise (sparkling ground, bright seams in clouds)
add_compile_options(-fsigned-char)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w -fvisibility=hidden)

# dav1d, the AV1 decoder (hle_movie_av1.c): its arm64 NEON assembly and the per-bit-depth
# wrappers in config/tmpl (the *_tmpl.c are not built on their own; the *_tmpl.S and util.S
# assembly others include are not built on their own). The config.h the sources include is the
# Android one in dm/android/dav1d (the checked-in third_party config/config.h is macOS/Meson).
set(DAV1D ${DM}/third_party/dav1d)
file(GLOB DAV1D_C ${DAV1D}/src/*.c)
list(FILTER DAV1D_C EXCLUDE REGEX "_tmpl\\.c$")
list(APPEND DAV1D_C ${DAV1D}/src/arm/cpu.c)
file(GLOB DAV1D_TMPL ${DAV1D}/config/tmpl/*.c)
list(APPEND DAV1D_C ${DAV1D_TMPL})
file(GLOB DAV1D_S ${DAV1D}/src/arm/64/*.S)
list(FILTER DAV1D_S EXCLUDE REGEX "_tmpl\\.S$")
list(FILTER DAV1D_S EXCLUDE REGEX "/util\\.S$")
add_library(dav1d STATIC ${DAV1D_C} ${DAV1D_S})
target_include_directories(dav1d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/dav1d ${DAV1D} ${DAV1D}/include ${DAV1D}/config)
target_compile_options(dav1d PRIVATE -O2 -fvisibility=hidden -fomit-frame-pointer -fno-align-functions)

# The replay records every draw on the game's render thread (descriptor writes, pipeline and view lookups, vertex and uniform copies): -O2 there, after the target's -O1
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/gfx_vk.c PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(${DM}/gfx/nvn/hle_nvn.c PROPERTIES COMPILE_OPTIONS "-fno-omit-frame-pointer")   # frame records: DM_NVN_SIZE_TRACE walks the game's call chain from them
add_library(dmcore OBJECT ${HOST_C} ${GFX_C} ${ANDROID_C} ${ASM_ELF})
add_dependencies(dmcore game_profile)
target_include_directories(dmcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include
  ${GEN} ${DM}/third_party/libvpx ${DM}/third_party/stb_vorbis ${DAV1D}/include)
# x18 holds the guest thread's slot block (dm.h dm_slot_get): the compiler must never use it (Android reserves it by default; stated anyway)
target_compile_options(dmcore PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-format -Wno-deprecated-declarations -Wno-gnu-folding-constant -Wno-multichar -Wno-four-char-constants -Wno-unknown-pragmas -Wno-missing-braces>
  -ffixed-x18 -fvisibility=hidden -fPIC -march=armv8.2-a+lse)
target_compile_definitions(dmcore PRIVATE DM_ANDROID=1)

set(DM_LIBS vpx dav1d log android aaudio mediandk vulkan m dl)

set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(dmhost_app SHARED $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_android.c ${CMAKE_CURRENT_SOURCE_DIR}/nvn_demo.c ${GLUE}/android_native_app_glue.c)
target_include_directories(dmhost_app PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES> ${GLUE})
target_compile_options(dmhost_app PRIVATE -include dm_android.h -ffixed-x18 -O1 -g -Wall)
target_link_libraries(dmhost_app ${DM_LIBS} jnigraphics)
target_link_options(dmhost_app PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384 -u ANativeActivity_onCreate)
set_target_properties(dmhost_app PROPERTIES OUTPUT_NAME dmhost)
add_library(nxloader SHARED ${CMAKE_CURRENT_SOURCE_DIR}/loader.c)
target_compile_options(nxloader PRIVATE -O2 -Wall -fvisibility=hidden)
target_link_libraries(nxloader log android dl)
target_link_options(nxloader PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)

add_executable(dmhost_cli $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_cli.c ${CMAKE_CURRENT_SOURCE_DIR}/nvn_demo.c)
set_target_properties(dmhost_cli PROPERTIES OUTPUT_NAME dmhost)
target_include_directories(dmhost_cli PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost_cli PRIVATE -include dm_android.h -ffixed-x18 -O1 -g -Wall)
target_link_libraries(dmhost_cli ${DM_LIBS})
target_link_options(dmhost_cli PRIVATE -Wl,-z,max-page-size=16384 -Wl,--error-limit=0)

# Host unit tests for adb shell (-DDM_TESTS=ON): the sources and flags of dm/host/tests/Makefile, with the Android stand-ins (plat_android.c)
option(DM_TESTS "build dm/host/tests for the device" OFF)
if(DM_TESTS)
  set(T ${DM}/host/tests)
  set(TEST_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/plat_android.c)
  function(dm_test name)
    cmake_parse_arguments(A "" "PROFILE" "SOURCES;DEFS" ${ARGN})
    add_executable(${name} ${A_SOURCES} ${TEST_COMMON})
    add_dependencies(${name} game_profile)
    if(A_PROFILE)
      target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${A_PROFILE})
    else()
      target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${GEN})
    endif()
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -Wno-unused-function -Wno-format -Wno-deprecated-declarations> -O1 -g -ffixed-x18)
    target_compile_definitions(${name} PRIVATE ${A_DEFS})
    target_link_libraries(${name} log)
  endfunction()
  dm_test(t_thread_leak SOURCES ${T}/t_thread_leak.c)
  dm_test(t_condvar SOURCES ${T}/t_condvar.c ${T}/t_condvar_stub.c ${DM}/host/hle_thread.c DEFS DM_TEST_HOOKS)
  dm_test(t_fs_commit_list SOURCES ${T}/t_fs_commit_list.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c)
  dm_test(t_svc_memory SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile)
  dm_test(t_svc_memory_unlimited SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile DEFS DM_GAME_MEM_MAP_LIMIT=0)
  # t_svc_memory_16k is not built: it simulates 16 KiB pages over the kernel's, which works where mmap aligns to 16 KiB (Apple silicon); a 4 KiB kernel gives
  # the guest address space only 4 KiB alignment. A 16 KiB-page Android kernel runs t_svc_memory itself at its real page size
  dm_test(t_nro SOURCES ${T}/t_nro.c)
  dm_test(t_audio SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_audio.c ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_aaudio.c)
  target_link_libraries(t_audio aaudio mediandk m)
  dm_test(t_media SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_media.c ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c)
  target_link_libraries(t_media mediandk android m)
  dm_test(t_hid SOURCES ${T}/t_hid.c ${DM}/host/hle_haptics.c)
  dm_test(t_preempt SOURCES ${T}/t_preempt.c ${T}/t_condvar_stub.c)
  dm_test(t_fs SOURCES ${T}/t_fs.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c)
  # The NVN interface layer's tests (dm/gfx/tests/t_nvn.c includes hle_nvn.c) against the GPU-less replay of gfx_null.c
  set(GFX_NO_IF ${GFX_C})
  list(REMOVE_ITEM GFX_NO_IF ${DM}/gfx/nvn/hle_nvn.c)
  dm_test(t_nvn SOURCES ${DM}/gfx/tests/t_nvn.c ${DM}/host/crash.c ${DM}/host/hle_bigstack.c ${GFX_NO_IF} ${CMAKE_CURRENT_SOURCE_DIR}/gfx_${DM_GFX}.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_android_stubs.c ${CMAKE_CURRENT_SOURCE_DIR}/apple_stubs.c ${GEN}/asm/nvn_entry.S ${GEN}/asm/nvn_thunks.S)
  target_include_directories(t_nvn PRIVATE ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include)
  target_compile_options(t_nvn PRIVATE -march=armv8.2-a+lse)
  target_link_libraries(t_nvn vulkan)
  target_link_options(t_nvn PRIVATE -Wl,--error-limit=0)
endif()
