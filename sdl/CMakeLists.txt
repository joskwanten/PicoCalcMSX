# Host (desktop) build of the MSX core with an SDL2 frontend — for development.
# Build:  cd sdl && cmake -B build && cmake --build build && ./build/msx_sdl
# Independent of the Pico SDK; reuses the platform-agnostic core in ../emu + ../zeta.

cmake_minimum_required(VERSION 3.16)
project(msx_sdl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL2 REQUIRED)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_executable(msx_sdl
    main_sdl.c
    storage_host.c
    ${ROOT}/menu.c
    ${ROOT}/emu/machine.c
    ${ROOT}/zeta/Z80.c
    ${ROOT}/emu/tms9918.c
    ${ROOT}/emu/PPI.c
    ${ROOT}/emu/slots.c
    ${ROOT}/emu/subslots.c
    ${ROOT}/emu/ram.c
    ${ROOT}/emu/rom.c
    ${ROOT}/emu/empty.c
    ${ROOT}/emu/util.c
    ${ROOT}/emu/konami-mega-rom-scc.c
    ${ROOT}/emu/emu2149.c
)

target_compile_definitions(msx_sdl PRIVATE Z80_STATIC)

target_include_directories(msx_sdl PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/compat   # pico.h stub
    ${ROOT}                            # storage.h
    ${ROOT}/emu
    ${ROOT}/zeta
)

target_link_libraries(msx_sdl SDL2::SDL2 SDL2::SDL2main)
