set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
# The win32 thread model (GCC 13+): std::mutex / std::condition_variable on the native SRW
# locks and condition variables. The posix model's winpthreads made each guest thread switch
# ~12x slower (77 us vs 6 us per handoff measured under Wine), which cost a large share of
# every game frame.
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-win32)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-win32)
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${CMAKE_SOURCE_DIR}/third_party/SDL2-2.30.9/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
