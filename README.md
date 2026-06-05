# msx-pulse

A diagnostic cartridge for MSX computers. Very much a work in progress. This is at
proof-of-concept stage. It boots, it reads the bus, it tells you things, but it's a
long way from a finished product. I'm building it partly because I wanted an "is this
MSX dead, and if so why" tool, and partly to learn, so expect rough edges.

## What it does today

Plug it into the cartridge slot and it sits there passively. It doesn't drive the bus,
so it's safe to leave in while the MSX powers up on its own. While it watches, it
reports on a small OLED:

- the +5V, +12V and −12V rails
- whether the 3.58 MHz clock is alive and roughly the right frequency
- the state of /RESET
- what the address and data lines are doing, including bits that look stuck or are
  never driven

It can also pulse /RESET and follow the early boot sequence to see how far the machine
gets. There's a USB serial log too if you want the detail on a host.

That's already enough to tell, say, a dead clock apart from dead RAM without pulling
the machine apart. I'm still working out the full list of faults it can sensibly
diagnose and a lot of that will come from pointing it at actually-broken machines and
seeing what shows up.

## What's next

The next goal is to have the cartridge *serve* a diagnostic ROM to the MSX and act as
the boot ROM itself and run Z80 test code on the machine's own CPU, the way the Dead
Test cart does. The firmware already has the groundwork for this (it can emulate a ROM
on the bus), but turning that into a proper guided test sequence is the next chunk of
work. The passive monitoring above is the part that somewhat works.

## Hardware

Fair warning: I'm a software person, not a hardware engineer, and this board is one of
the first things I've designed. It's a quick first cut to get *something* I could test
firmware against, and it shows. There's a KiCad project in [`hardware/`](hardware/)
(schematics, PCB, gerbers) and a parts list in [`docs/BOM.csv`](docs/BOM.csv) — read
them as "what I actually had made", not "the right way to do this".

The brain is a bare RP2354B (the QFN-80 part, 48 GPIO, 2 MB internal flash) wired onto
the cartridge edge connector and powered from the slot. The current board puts 74LVC245
level shifters on just about every bus line. Having lived with it, I think that's
overkill... the address and control inputs could most likely get by with plain series
resistors and no shifters. The data lines are the exception: those are bidirectional
and actually get driven, so keeping real level shifting there still makes sense, mostly
for protection.

Things I'd like to change in the next board revision:

- drop the level shifters on the address/control inputs, keep them on D0–D7
- shrink the PCB to fit an actual MSX cartridge shell (it's oversized right now)
- swap the ADC protection diodes as I miscalculated the zeners, seems it works even better without.
- a bigger power-on-reset cap for a more reliable cold boot
- a normal 2.54 mm SWD header (plus a UART header which I omitted) instead of the fine-pitch one I used

## Building

You'll need the Arm GNU toolchain (`arm-none-eabi`), the Raspberry Pi Pico SDK, and
sjasmplus for the Z80 side.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -DPICO_TOOLCHAIN_PATH=/path/to/arm-none-eabi/bin
cmake --build build -j
```

Out comes:

- `build/src/firmware/msxdoc.uf2` — the firmware
- `build/diag.rom` — the Z80 test ROM, assembled and baked into the firmware for you (not served) 

## Flashing

Hold BOOTSEL, plug into USB, and drop the `.uf2` onto the drive that appears. Or, with
picotool:

```sh
picotool load -x build/src/firmware/msxdoc.uf2
```

Or via swd.

## Layout

```
src/rom/        Z80 test ROM (sjasmplus)
src/firmware/   RP2354B firmware (pico-sdk, C + PIO)
hardware/       KiCad schematics, PCB, gerbers
docs/           bill of materials
```

## License

MIT — see [LICENSE](LICENSE).
