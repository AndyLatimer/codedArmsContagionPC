cmake_minimum_required(VERSION 3.16)
project(CodedArmsContagion VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SDL2: system package on Linux; bundled MinGW/VC development package on Windows.
if(WIN32 AND NOT SDL2_DIR)
  if(MINGW AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/SDL2-2.30.9/cmake")
    set(SDL2_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL2-2.30.9/cmake")
  elseif(MSVC AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/SDL2-VC/cmake")
    set(SDL2_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL2-VC/cmake")
  endif()
endif()
find_package(SDL2 REQUIRED CONFIG)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Front end support library (config, log, input, GL loader); linked by the game and the tests.
add_library(ca_engine STATIC
  src/core/Log.cpp
  src/core/Config.cpp
  src/platform/Input.cpp
  src/render/GL.cpp
)
target_include_directories(ca_engine PUBLIC src)
target_include_directories(ca_engine SYSTEM PUBLIC third_party/stb)
target_link_libraries(ca_engine PUBLIC SDL2::SDL2)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME inspect_iso COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_inspect_iso.py)

# ---- Recompiled game (static recompilation of the user's own BOOT.BIN) ----------------
# Configure with -DCA_GAME_ISO=/path/to/your/Coded.Arms.Contagion.iso. The generated C is derived from
# the game and stays inside the build directory; it is never committed or distributed.
set(CA_GAME_ISO "" CACHE FILEPATH "Your own Coded Arms: Contagion (ULES-00718) ISO, needed for the recompiled build")
set(CA_RECOMP_TRACE "" CACHE STRING "Debug: comma separated guest function addresses to trace")
option(CA_RECOMP_WATCH "Debug: generate store watchpoints (CA_WATCH_ADDR at run time)" OFF)
option(CA_RECOMP_PROFILE "Debug: count calls of every recompiled function (runner command profdump)" OFF)
add_library(ca_psp OBJECT
  src/psp/cpu.cpp
  src/psp/vfpu.cpp
  src/psp/memory.cpp
  src/psp/iso.cpp
  src/psp/kernel.cpp
  src/psp/hle_io.cpp
  src/psp/hle_devices.cpp
  src/psp/hle_sas.cpp
  src/games/ULES00718/game_hooks.cpp
  src/psp/ppsspp/SasAudio.cpp
  src/psp/ppsspp/SasReverb.cpp
  src/psp/hle_nids.cpp
  src/psp/ge.cpp
  src/psp/gpu/gpu.cpp
  src/psp/gpu/ads.cpp
  src/psp/gpu/button_glyphs.cpp
  src/psp/gpu/minimap.cpp
  src/psp/gpu/vertex_decoder.cpp
  src/psp/gpu/texture_decoder.cpp
  src/render/GL.cpp
  src/psp/ppsspp/InterpreterVFPU.cpp
  src/psp/ppsspp/MIPSVFPUUtils.cpp
  # sceUtility savedata / message dialogs (PPSSPP port) and the host dialog interface
  src/psp/hle_utility.cpp
  src/psp/host_dialog.cpp
  src/psp/ppsspp/ppsspp_utility_compat.cpp
  src/psp/ppsspp/PSPDialog.cpp
  src/psp/ppsspp/PSPMsgDialog.cpp
  src/psp/ppsspp/PSPSaveDialog.cpp
  src/psp/ppsspp/SavedataParam.cpp
  src/psp/ppsspp/ParamSFO.cpp
  src/psp/ppsspp/sceChnnlsv.cpp
)
target_include_directories(ca_psp PUBLIC src src/psp/ppsspp)
target_include_directories(ca_psp SYSTEM PRIVATE third_party/stb)
target_link_libraries(ca_psp PUBLIC SDL2::SDL2)
target_compile_options(ca_psp PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers>)
find_package(Threads REQUIRED)

# ---- sceMpeg (PSMF movies): PPSSPP's MediaEngine with FFmpeg for H.264 video and the
# standalone ATRAC3+ decoder for audio. FFmpeg is a minimal static build made by
# tools/build_ffmpeg.sh (third_party/ffmpeg/<platform>).
if(MINGW)
  set(CA_FFMPEG_PLATFORM mingw-x86_64)
elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(CA_FFMPEG_PLATFORM linux-x86_64)
endif()
set(CA_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/${CA_FFMPEG_PLATFORM}" CACHE PATH "Static FFmpeg (include/, lib/) for movie playback")
if(NOT EXISTS "${CA_FFMPEG_DIR}/lib")
  message(FATAL_ERROR "No FFmpeg for this platform in ${CA_FFMPEG_DIR}; build one with tools/build_ffmpeg.sh")
endif()
add_library(ca_ffmpeg INTERFACE)
target_include_directories(ca_ffmpeg SYSTEM INTERFACE ${CA_FFMPEG_DIR}/include)
target_link_libraries(ca_ffmpeg INTERFACE
  ${CA_FFMPEG_DIR}/lib/libavformat.a ${CA_FFMPEG_DIR}/lib/libavcodec.a
  ${CA_FFMPEG_DIR}/lib/libswscale.a ${CA_FFMPEG_DIR}/lib/libavutil.a)
if(WIN32)
  target_link_libraries(ca_ffmpeg INTERFACE bcrypt advapi32)
else()
  target_link_libraries(ca_ffmpeg INTERFACE m)
endif()
file(GLOB CA_AT3_SOURCES ${CMAKE_SOURCE_DIR}/src/psp/ppsspp/at3_standalone/*.cpp)
add_library(ca_at3 STATIC ${CA_AT3_SOURCES})
target_include_directories(ca_at3 PRIVATE src/psp/ppsspp/at3_standalone/shim)
if(NOT MSVC)
  target_compile_options(ca_at3 PRIVATE -w)
endif()
target_sources(ca_psp PRIVATE
  src/psp/hle_mpeg.cpp
  src/psp/ppsspp/MediaEngine.cpp
  src/psp/ppsspp/MpegDemux.cpp
  src/psp/ppsspp/Atrac3Standalone.cpp)
target_include_directories(ca_psp SYSTEM PRIVATE ${CA_FFMPEG_DIR}/include)
target_compile_definitions(ca_psp PRIVATE USE_FFMPEG)

# KIRK crypto engine from PPSSPP's ext/libkirk (savedata encryption), see third_party/libkirk.
add_library(ca_kirk OBJECT third_party/libkirk/kirk_engine.c third_party/libkirk/AES.c
  third_party/libkirk/SHA1.c third_party/libkirk/bn.c third_party/libkirk/ec.c)
if(NOT MSVC)
  target_compile_options(ca_kirk PRIVATE -w)
endif()
target_include_directories(ca_psp PUBLIC third_party/libkirk)

# ---- Savedata tests (no game image needed). With -DCA_PPSSPP_DIR=<PPSSPP checkout> the test
# also checks the encryption and hashes against PPSSPP's own sceChnnlsv.cpp.
set(CA_PPSSPP_DIR "" CACHE PATH "Optional PPSSPP source checkout used as reference by test_savedata")
add_executable(test_savedata tests/savedata/test_savedata.cpp src/psp/memory.cpp src/psp/host_dialog.cpp
  src/psp/ppsspp/ppsspp_utility_compat.cpp src/psp/ppsspp/PSPDialog.cpp src/psp/ppsspp/SavedataParam.cpp
  src/psp/ppsspp/ParamSFO.cpp src/psp/ppsspp/sceChnnlsv.cpp $<TARGET_OBJECTS:ca_kirk>)
target_include_directories(test_savedata PRIVATE src src/psp/ppsspp third_party/libkirk)
if(CA_PPSSPP_DIR)
  target_sources(test_savedata PRIVATE tests/savedata/ref_chnnlsv.cpp)
  set_source_files_properties(tests/savedata/ref_chnnlsv.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/tests/savedata/refstub;${CA_PPSSPP_DIR}")
  target_compile_definitions(test_savedata PRIVATE CA_HAVE_PPSSPP_REF=1)
endif()
add_test(NAME savedata COMMAND test_savedata ${CMAKE_BINARY_DIR}/savedata_test_ms)

if(CA_GAME_ISO)
  set(CA_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
  set(CA_WATCH_ARG "")
  if(CA_RECOMP_WATCH)
    set(CA_WATCH_ARG --watch)
  endif()
  if(CA_RECOMP_PROFILE)
    list(APPEND CA_WATCH_ARG --profile)
  endif()
  message(STATUS "Recompiling BOOT.BIN from ${CA_GAME_ISO}")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp/recompile.py --iso ${CA_GAME_ISO} --out ${CA_GEN_DIR} --trace=${CA_RECOMP_TRACE} ${CA_WATCH_ARG}
    RESULT_VARIABLE CA_RECOMP_RESULT)
  if(NOT CA_RECOMP_RESULT EQUAL 0)
    message(FATAL_ERROR "Recompilation failed")
  endif()
  # Regenerate when the recompiler or the port hooks change.
  file(GLOB CA_RECOMP_INPUTS ${CMAKE_SOURCE_DIR}/tools/recomp/*.py ${CMAKE_SOURCE_DIR}/tools/recomp/games/*/*.tsv)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CA_RECOMP_INPUTS})
  file(GLOB CA_GEN_SOURCES ${CA_GEN_DIR}/*.c)
  add_library(ca_guest OBJECT ${CA_GEN_SOURCES})
  target_include_directories(ca_guest PRIVATE src ${CA_GEN_DIR})
  if(NOT MSVC)
    target_compile_options(ca_guest PRIVATE -w -fwrapv -fno-strict-aliasing)
  endif()

  # The game: recompiled code + HLE runtime + native front end.
  add_executable(CodedArmsContagion WIN32 src/frontend/Main.cpp src/frontend/StbImpl.cpp src/frontend/DialogOverlay.cpp src/frontend/OptionsMenu.cpp src/frontend/CrashLog.cpp $<TARGET_OBJECTS:ca_psp> $<TARGET_OBJECTS:ca_kirk> $<TARGET_OBJECTS:ca_guest>)
  target_include_directories(CodedArmsContagion PRIVATE src)
  target_include_directories(CodedArmsContagion SYSTEM PRIVATE third_party/stb)
  if(TARGET SDL2::SDL2main)
    target_link_libraries(CodedArmsContagion PRIVATE SDL2::SDL2main)
  endif()
  target_link_libraries(CodedArmsContagion PRIVATE ca_engine ca_at3 ca_ffmpeg Threads::Threads SDL2::SDL2)
  if(MINGW)
    # Threads created without an explicit size get the executable's stack reserve: 1 MB by
    # default on Windows (8 MB on Linux); every guest thread runs on one. The map file turns
    # crash report offsets (CodedArmsContagion.exe+0x...) into functions.
    target_link_options(CodedArmsContagion PRIVATE -static-libgcc -static-libstdc++ -static -Wl,--stack,16777216
                        -Wl,-Map=${CMAKE_BINARY_DIR}/CodedArmsContagion.map)
  endif()

  add_executable(ca_psprun src/psp/psprun.cpp $<TARGET_OBJECTS:ca_psp> $<TARGET_OBJECTS:ca_kirk> $<TARGET_OBJECTS:ca_guest>)
  target_include_directories(ca_psprun PRIVATE src)
  target_link_libraries(ca_psprun PRIVATE ca_at3 ca_ffmpeg Threads::Threads SDL2::SDL2)
  target_include_directories(ca_psprun SYSTEM PRIVATE third_party/stb)
  if(MINGW)
    target_link_options(ca_psprun PRIVATE -static-libgcc -static-libstdc++ -static -Wl,--stack,16777216)
  endif()
endif()

# ---- Differential CPU test (needs the game image like the recompiled build) ----------------
if(CA_GAME_ISO)
  set(CA_CPU_TESTS ${CA_GEN_DIR}/cpu_tests.c)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import sys; sys.path.insert(0, '${CMAKE_SOURCE_DIR}/tools/recomp'); from iso9660 import read_iso_file; open('${CA_GEN_DIR}/BOOT.BIN','wb').write(read_iso_file('${CA_GAME_ISO}', 'PSP_GAME/SYSDIR/BOOT.BIN'))")
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp/gen_cpu_tests.py ${CA_GEN_DIR}/BOOT.BIN ${CA_CPU_TESTS})
  add_executable(cpu_diff tests/cpu_diff/cpu_diff.cpp tests/cpu_diff/ref_interp.cpp ${CA_CPU_TESTS}
                 src/psp/cpu.cpp src/psp/memory.cpp)
  target_include_directories(cpu_diff PRIVATE src tests/cpu_diff)
  if(NOT MSVC)
    set_source_files_properties(${CA_CPU_TESTS} PROPERTIES COMPILE_OPTIONS "-w;-fwrapv;-fno-strict-aliasing")
  endif()
endif()
