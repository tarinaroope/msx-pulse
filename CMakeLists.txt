cmake_minimum_required(VERSION 3.13)

# Target platform — must be set before pico_sdk_import and project()
set(PICO_PLATFORM rp2350-arm-s)

# Target chip is RP2354B (RP2350B family, QFN-80, 48 GPIOs). Without
# overriding PICO_BOARD, pico-sdk defaults to `pico2`, whose board file
# `pico2.h` defines `PICO_RP2350A 1` UNCONDITIONALLY (no #ifndef guard) —
# command-line -D can't override it. With PICO_RP2350A=1, NUM_BANK0_GPIOS
# is 30, and `gpio_get`/`gpio_put` for pins ≥32 silently misbehave because
# `mask = 1u << pin` is UB on a 32-bit register. This broke the LED (GP38),
# /RESET monitoring (GP32), /RESET drive (GP33), and all three panel
# buttons (GP39/43/44).
#
# `pimoroni_pga2350` is also an RP2350B QFN-80 board file and sets
# `PICO_RP2350A 0`. We use it purely for NUM_BANK0_GPIOS=48 — our firmware
# overrides any Pimoroni-specific pin defaults explicitly.
set(PICO_BOARD pimoroni_pga2350)

# RP2354B internal flash is 2 MB. Without this override the SDK defaults to
# Pico 2's 4 MB on rp2350-arm-s, which makes the linker map FLASH to 4 MB and
# causes PICO_FLASH_SIZE_BYTES-relative reads (mode_store at last sector) to
# land at 0x103FF000 — past the actual chip's flash window. RP2350 QMI behavior
# on out-of-range reads is hardware-dependent and unsafe to rely on.
set(PICO_FLASH_SIZE_BYTES 0x200000)

# Must be included before project()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(msxdoc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ── Z80 ROM ───────────────────────────────────────────────────────────────────
find_program(SJASMPLUS sjasmplus REQUIRED)

# Font: generated from src/firmware/oled.c so MSX Screen 0 text matches OLED.
# INCBIN in main.asm resolves relative to the sjasmplus working directory
# (${CMAKE_BINARY_DIR}), so font.bin is emitted there.
add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/font.bin
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_msx_font.py
            ${CMAKE_SOURCE_DIR}/src/firmware/oled.c
            ${CMAKE_BINARY_DIR}/font.bin
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_msx_font.py
            ${CMAKE_SOURCE_DIR}/src/firmware/oled.c
    COMMENT "Generating MSX row-major font from oled.c"
)

# ── Z80 ROM selection ────────────────────────────────────────────────────────
# MSXDOC_ROM=diag (default) → src/rom/main.asm    → full diagnostic suite
# MSXDOC_ROM=minimal        → src/rom/minimal.asm → 30-byte cart_serve test
set(MSXDOC_ROM "diag" CACHE STRING "Which Z80 ROM to embed: diag or minimal")
set_property(CACHE MSXDOC_ROM PROPERTY STRINGS diag minimal)

if(MSXDOC_ROM STREQUAL "diag")
    set(ROM_SRC ${CMAKE_SOURCE_DIR}/src/rom/main.asm)
elseif(MSXDOC_ROM STREQUAL "minimal")
    set(ROM_SRC ${CMAKE_SOURCE_DIR}/src/rom/minimal.asm)
else()
    message(FATAL_ERROR "MSXDOC_ROM must be 'diag' or 'minimal' (got '${MSXDOC_ROM}')")
endif()

add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/diag.rom
    COMMAND ${SJASMPLUS} ${ROM_SRC}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${ROM_SRC} ${CMAKE_BINARY_DIR}/font.bin
    COMMENT "Assembling Z80 ROM (${MSXDOC_ROM})"
)
add_custom_target(rom ALL DEPENDS ${CMAKE_BINARY_DIR}/diag.rom)

# ── Firmware git short SHA (for self-test reporting) ─────────────────────────
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE   FW_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT FW_GIT_SHA)
    set(FW_GIT_SHA "unknown")
endif()

# ── RP2350B firmware ──────────────────────────────────────────────────────────
add_subdirectory(src/firmware)

# Firmware depends on ROM: diag.rom must exist before rom_image.h is generated
add_dependencies(msxdoc rom)
