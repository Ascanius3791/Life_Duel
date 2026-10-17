cmake_minimum_required(VERSION 3.16)
project(GameOfLife LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Tune for the building machine (AVX2/AVX-512, POPCNT). Makes the battle engine
# used for bot training several times faster, but the binaries may not run on
# older CPUs. Turn off for portable builds: -DGOL_NATIVE=OFF.
option(GOL_NATIVE "Optimise for the CPU of the building machine (-march=native)" ON)
if(GOL_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native GOL_HAS_MARCH_NATIVE)
  if(GOL_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  endif()
endif()

# --- Shared simulation core -------------------------------------------------
add_library(gol_core
  core/src/life.cpp
  core/src/patterns.cpp
  core/src/battle.cpp
  core/src/fast_battle.cpp
)
target_include_directories(gol_core PUBLIC core/include)

# --- WebAssembly build for the static web site (GitHub Pages) ---------------
# Configured with Emscripten (emcmake cmake -B build-wasm), only the battle
# engine and the core tests are built; tools/build_site.sh assembles the site.
if(EMSCRIPTEN)
  add_executable(life_duel_engine battle/wasm_engine.cpp battle/api_json.cpp)
  target_link_libraries(life_duel_engine PRIVATE gol_core)
  target_link_options(life_duel_engine PRIVATE
    -sMODULARIZE=1 -sEXPORT_NAME=createLifeDuelEngine -sENVIRONMENT=worker,node
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432
    "-sEXPORTED_FUNCTIONS=['_engine_config','_engine_battlefield','_engine_battle','_engine_step','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8']")

  # The same core tests, run under node: the engine must behave exactly as natively.
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE gol_core)
  target_link_options(core_tests PRIVATE -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
  add_test(NAME core_tests COMMAND core_tests)
  return()
endif()

# --- Classic Game of Life with a native GUI (SFML) -------------------------
find_package(SFML 2.5 COMPONENTS graphics window system QUIET)
if(SFML_FOUND)
  add_executable(life_classic classic/main.cpp classic/ui.cpp)
  target_link_libraries(life_classic PRIVATE gol_core sfml-graphics sfml-window sfml-system)
  target_compile_definitions(life_classic PRIVATE
    GOL_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
  add_custom_command(TARGET life_classic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:life_classic>/assets)
else()
  message(WARNING "SFML 2.5 not found - skipping life_classic (install libsfml-dev)")
endif()

# --- 1v1 colour battle, served to the browser -------------------------------
find_package(Threads REQUIRED)
add_executable(life_battle battle/main.cpp battle/api_json.cpp battle/http_server.cpp)
target_link_libraries(life_battle PRIVATE gol_core Threads::Threads)
target_compile_definitions(life_battle PRIVATE
  GOL_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/battle/web"
  GOL_LEAGUE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/leagues")
add_custom_command(TARGET life_battle POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/battle/web $<TARGET_FILE_DIR:life_battle>/web
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets/leagues $<TARGET_FILE_DIR:life_battle>/leagues)

# --- Bot training: self-play league on a fixed battlefield --------------------
add_library(gol_arena arena/arena.cpp arena/league.cpp)
target_include_directories(gol_arena PUBLIC arena)
target_link_libraries(gol_arena PUBLIC gol_core Threads::Threads)
add_executable(life_arena arena/main.cpp)
target_link_libraries(life_arena PRIVATE gol_arena)

# --- Tests ------------------------------------------------------------------
enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE gol_core)
add_test(NAME core_tests COMMAND core_tests)
add_executable(arena_tests tests/arena_tests.cpp)
target_link_libraries(arena_tests PRIVATE gol_arena)
add_test(NAME arena_tests COMMAND arena_tests)
