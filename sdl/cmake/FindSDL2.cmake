# Fallback vinder voor SDL2-installaties zonder CMake-configbestanden
# (bijv. het scoop 'sdl2'-pakket: dat pakt de VC-devel-zip uit maar gooit de
# cmake/-map weg, waardoor CONFIG-mode niets vindt).
#
# Probeert eerst de officiële CONFIG-mode; lukt dat niet, dan worden headers en
# libs gezocht via CMAKE_PREFIX_PATH (die zet de scoop-installer als user-envvar).

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    set(SDL2_FOUND TRUE)
    return()
endif()

find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES include/SDL2 SDL2)
find_library(SDL2_LIBRARY NAMES SDL2)
find_library(SDL2MAIN_LIBRARY NAMES SDL2main)
find_file(SDL2_RUNTIME_DLL SDL2.dll PATH_SUFFIXES lib bin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2 REQUIRED_VARS SDL2_LIBRARY SDL2_INCLUDE_DIR)

if(NOT SDL2_FOUND)
    return()
endif()

add_library(SDL2::SDL2 UNKNOWN IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
    IMPORTED_LOCATION "${SDL2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")

# SDL2main.lib uit de VC-zip is met MSVC gecompileerd; met MinGW-gcc linkt die
# niet betrouwbaar. Sla hem daar over en laat SDL.h 'main' met rust via
# SDL_MAIN_HANDLED (main_sdl.c roept dan zelf SDL_SetMainReady() aan).
if(MSVC AND SDL2MAIN_LIBRARY)
    add_library(SDL2::SDL2main STATIC IMPORTED)
    set_target_properties(SDL2::SDL2main PROPERTIES
        IMPORTED_LOCATION "${SDL2MAIN_LIBRARY}")
else()
    add_library(SDL2::SDL2main INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2main PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS SDL_MAIN_HANDLED)
endif()
