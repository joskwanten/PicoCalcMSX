# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(BareMSX C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(BareMSX
        main_pico.c
        # Beelduitvoer: HDMI/DVI via RP2350 HSTX (pico_hdmi) + audio data islands
        video_hstx.c
        audio_hdmi.c
        # MSX emulator core (ported from sdl-msx)
        emu/machine.c
        zeta/Z80.c
        emu/tms9918.c
        emu/PPI.c
        emu/slots.c
        emu/subslots.c
        emu/ram.c
        emu/rom.c
        emu/empty.c
        emu/util.c
        emu/konami-mega-rom-scc.c
        emu/mapper.c
        emu/v9938.c
        emu/rtc.c
        emu/mappedram.c
        emu/wd2793.c
        emu/diskrom.c
        emu/emu2149.c
        )

pico_set_program_name(BareMSX "BareMSX")
pico_set_program_version(BareMSX "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(BareMSX 1)
pico_enable_stdio_usb(BareMSX 0)

# Add the standard library to the build
target_link_libraries(BareMSX
        pico_stdlib
        pico_multicore)

# Add the standard include files to the build
target_include_directories(BareMSX PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/emu
        ${CMAKE_CURRENT_LIST_DIR}/zeta
)

# Zeta Z80 core wordt statisch mee-gecompileerd
target_compile_definitions(BareMSX PRIVATE Z80_STATIC)

# Een gefaalde malloc moet NULL teruggeven (het ROM-laadpad handelt dat af),
# niet panicken (SDK-default) — dat gaf een stille lockup bij grote ROMs.
target_compile_definitions(BareMSX PRIVATE PICO_MALLOC_PANIC=0)

# MSX2-profiel (V9938 + mapper-RAM + RTC); boot als system/ zowel msx2* als
# msx2ext* bevat. Het V9938-VRAM hergebruikt de 128KB menu-arena.
target_compile_definitions(BareMSX PRIVATE BAREMSX_MSX2=1)

# Ingebakken fallback-ROMs (emu/bios_rom.h + emu/nemesis2.h; zelf genereren met
# tools/rom_to_header.py — zitten niet in git). Zonder deze optie bevat de
# binary géén copyrighted materiaal (publiceerbare build) en is een SD-kaart
# met een BIOS in system/ vereist. Default: aan als de headers bestaan.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/emu/bios_rom.h AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/emu/nemesis2.h)
    set(_baked_default ON)
else()
    set(_baked_default OFF)
endif()
option(BAREMSX_BAKED_ROMS "Bake fallback BIOS+game ROM headers into the binary" ${_baked_default})
if(BAREMSX_BAKED_ROMS)
    target_compile_definitions(BareMSX PRIVATE BAREMSX_BAKED_ROMS=1)
endif()

# USB-keyboard via TinyUSB-host op de native USB-poort.
option(BAREMSX_USB_KEYBOARD "Read a USB keyboard via TinyUSB host" ON)
if(BAREMSX_USB_KEYBOARD)
    target_sources(BareMSX PRIVATE usbkbd.c)
    target_link_libraries(BareMSX tinyusb_host tinyusb_board)
    target_compile_definitions(BareMSX PRIVATE BAREMSX_USB_KEYBOARD=1)
endif()

# SD-card game-loading + boot-menu (FatFs op SPI0, GP2-5).
option(BAREMSX_SD "SD-card game loading + boot menu (FatFs)" ON)
if(BAREMSX_SD)
    target_sources(BareMSX PRIVATE
            menu.c
            storage_pico.c
            sd_spi.c
            diskio.c
            flash_stage.c
            third_party/fatfs/ff.c
            third_party/fatfs/ffunicode.c
            third_party/fatfs/ffsystem.c
    )
    target_include_directories(BareMSX PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs)
    target_link_libraries(BareMSX hardware_spi hardware_flash hardware_watchdog)
    target_compile_definitions(BareMSX PRIVATE BAREMSX_SD=1)
endif()

# HDMI/DVI over HSTX (pico_hdmi):
# 252 MHz sysclk + HSTX-deler 2 -> zelfde 25.2 MHz pixelklok, 2x CPU-headroom.
set(PICO_HDMI_HSTX_CLK_DIV 2 CACHE STRING "HSTX clk div (252MHz/2 -> 25.2MHz pixel)" FORCE)
add_subdirectory(third_party/pico_hdmi)
target_link_libraries(BareMSX pico_hdmi)

pico_add_extra_outputs(BareMSX)

