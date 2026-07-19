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

project(PicoCalcMSX C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

# Beelduitvoer: HDMI/DVI via RP2350 HSTX (pico_hdmi), of de PicoCalc ILI9488-LCD.
option(PICOCALC_HDMI "Output over HDMI/DVI (RP2350 HSTX) instead of the SPI LCD" ON)

# De PicoCalc-keyboard (STM32 op I2C1) bestaat alleen mét het PicoCalc-moederbord.
# Een standalone HDMI-bord heeft (nog) geen keyboard — USB-HID is toekomstig werk.
# Zonder PicoCalc hangt de I2C-init, dus in de HDMI-build standaard uit.
if(PICOCALC_HDMI)
    set(_kbd_default OFF)
else()
    set(_kbd_default ON)
endif()
option(PICOCALC_I2C_KEYBOARD "Use the PicoCalc STM32 I2C keyboard" ${_kbd_default})

add_executable(PicoCalcMSX
        PicoCalcMSX.c
        i2ckbd.c
        keymap.c
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
        emu/emu2149.c
        )

pico_set_program_name(PicoCalcMSX "PicoCalcMSX")
pico_set_program_version(PicoCalcMSX "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(PicoCalcMSX 1)
pico_enable_stdio_usb(PicoCalcMSX 0)

# Add the standard library to the build
target_link_libraries(PicoCalcMSX
        pico_stdlib
        pico_multicore)

# Add the standard include files to the build
target_include_directories(PicoCalcMSX PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/emu
        ${CMAKE_CURRENT_LIST_DIR}/zeta
)

# Zeta Z80 core wordt statisch mee-gecompileerd
target_compile_definitions(PicoCalcMSX PRIVATE Z80_STATIC)

if(PICOCALC_I2C_KEYBOARD)
    target_compile_definitions(PicoCalcMSX PRIVATE PICOCALC_I2C_KEYBOARD=1)
endif()

# USB-keyboard via TinyUSB-host op de native USB-poort (standalone HDMI-bord).
option(PICOCALC_USB_KEYBOARD "Read a USB keyboard via TinyUSB host" ON)
if(PICOCALC_USB_KEYBOARD)
    target_sources(PicoCalcMSX PRIVATE usbkbd.c)
    target_link_libraries(PicoCalcMSX tinyusb_host tinyusb_board)
    target_compile_definitions(PicoCalcMSX PRIVATE PICOCALC_USB_KEYBOARD=1)
endif()

# SD-card game-loading + boot-menu (FatFs op SPI0, GP2-5).
option(PICOCALC_SD "SD-card game loading + boot menu (FatFs)" ON)
if(PICOCALC_SD)
    target_sources(PicoCalcMSX PRIVATE
            menu.c
            storage_pico.c
            sd_spi.c
            diskio.c
            third_party/fatfs/ff.c
            third_party/fatfs/ffunicode.c
            third_party/fatfs/ffsystem.c
    )
    target_include_directories(PicoCalcMSX PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs)
    target_link_libraries(PicoCalcMSX hardware_spi)
    target_compile_definitions(PicoCalcMSX PRIVATE PICOCALC_SD=1)
endif()

# Backend-specifieke sources + libraries
if(PICOCALC_HDMI)
    target_sources(PicoCalcMSX PRIVATE
            video_hstx.c
            audio_hdmi.c
    )
    # 252 MHz sysclk + HSTX-deler 2 -> zelfde 25.2 MHz pixelklok, 2x CPU-headroom.
    set(PICO_HDMI_HSTX_CLK_DIV 2 CACHE STRING "HSTX clk div (252MHz/2 -> 25.2MHz pixel)" FORCE)
    add_subdirectory(third_party/pico_hdmi)
    target_link_libraries(PicoCalcMSX pico_hdmi)
    target_compile_definitions(PicoCalcMSX PRIVATE PICOCALC_HDMI=1)
else()
    target_sources(PicoCalcMSX PRIVATE
            lcd.c
            audio.c
    )
    target_link_libraries(PicoCalcMSX hardware_spi hardware_pwm)
endif()

# Gemeenschappelijke libraries
target_link_libraries(PicoCalcMSX
        hardware_i2c
        hardware_dma
        pico_cyw43_arch_none
        )

pico_add_extra_outputs(PicoCalcMSX)

