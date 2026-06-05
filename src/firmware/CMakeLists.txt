# ── Embed Z80 ROM binary as a C header ────────────────────────────────────
# Requires: xxd (ships with vim on macOS; brew install vim if missing)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/rom_image.h
    COMMAND xxd -i -n rom_image_data ${CMAKE_BINARY_DIR}/diag.rom
            > ${CMAKE_CURRENT_BINARY_DIR}/rom_image.h
    DEPENDS ${CMAKE_BINARY_DIR}/diag.rom
    COMMENT "Generating rom_image.h from diag.rom"
)
add_custom_target(rom_image_h DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/rom_image.h)

# ── Firmware executable ─────────────────────────────────────────────────────
add_executable(msxdoc
    main.c
    cart_serve.c
    alog.c
    cmd_dispatch.c
    rom_lut.c
    boot_detect.c
    boot_capture.c
    msx_runtime.c
    msx_autoreset.c
    oled.c
    ui.c
    usb_serial.c
    usb_descriptors.c
    adc_rails.c
    buttons.c
    bus_probe.c
    bus_integrity.c
    txn_capture.c
    boot_diag.c
)

# FW_VERSION and FW_GIT_SHA are consumed by the on-device test banner.
target_compile_definitions(msxdoc PRIVATE
    FW_VERSION="v0.1"
    FW_GIT_SHA="${FW_GIT_SHA}"
    # Required so the SDK accepts PIO pin references > 31 (e.g. GP34 in the
    # cart_read side-set config). Defaults to 1 for rp2350-arm-s via
    # NUM_BANK0_GPIOS > 32, but make it explicit so a future RP2350A
    # retarget doesn't silently regress GP34 drive.
    PICO_PIO_USE_GPIO_BASE=1
)

# Generate PIO headers — must come after add_executable
pico_generate_pio_header(msxdoc ${CMAKE_CURRENT_SOURCE_DIR}/pios/cart_serve.pio)
pico_generate_pio_header(msxdoc ${CMAKE_CURRENT_SOURCE_DIR}/pios/boot_detect.pio)
pico_generate_pio_header(msxdoc ${CMAKE_CURRENT_SOURCE_DIR}/pios/txn_capture.pio)

# Generated header directory must be on the include path
target_include_directories(msxdoc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(msxdoc
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_adc
    hardware_i2c
    hardware_pwm
    tinyusb_device
    tinyusb_board
)

# Use TinyUSB directly — NOT pico_stdio_usb (incompatible with direct tusb_init)
pico_enable_stdio_uart(msxdoc 0)

pico_add_extra_outputs(msxdoc)

# msxdoc must build after the ROM target (provides diag.rom)
add_dependencies(msxdoc rom_image_h)
