SHELL    = /usr/bin/env bash
SKETCH   = OpenElevator.ino
FQBN     = teensy:avr:teensy41
BAUD     = 115200

# Auto-detect Teensy serial port. Override: make upload PORT=/dev/ttyACM1
PORT     ?= $(firstword $(wildcard /dev/ttyACM*))

.PHONY: all compile upload monitor flash clean help

all: compile

## compile: build the sketch.
## The arduino-cli in this shell has the Teensy core and U8g2 baked in
## via Nix -- no downloads, no sketch.yaml profile needed.
compile:
	arduino-cli compile -v \
		--fqbn $(FQBN) \
		--build-property "compiler.cpp.extra_flags=\"-DGIT_VERSION=\"$(shell git rev-parse --short HEAD)\"\""  \
		$(SKETCH)

## upload: compile and flash.
## Requires PJRC udev rules on Linux: https://www.pjrc.com/teensy/00-teensy.rules
upload: compile
	[ -e "$(PORT)" ] && \
	arduino-cli upload -v -p $(PORT) --fqbn $(FQBN) $(SKETCH) || \
	{ echo "==> $(PORT) not available. Is the Teensy plugged in?"; exit 1; }

## flash: upload last build artifact without recompiling.
flash:
	[ -e "$(PORT)" ] && \
	arduino-cli upload -v -p $(PORT) --fqbn $(FQBN) $(SKETCH) || \
	{ echo "==> $(PORT) not available. Is the Teensy plugged in?"; exit 1; }

## monitor: open picocom. Ctrl-A Ctrl-X to exit.
monitor:
	picocom -b $(BAUD) --imap lfcrlf $(PORT)

## clean: remove build artifacts.
clean:
	rm -rf build/

## help: list targets.
help:
	@grep -E '^##' Makefile | sed 's/## /  /'