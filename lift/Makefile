SKETCH      := FilmLift.ino
PROFILE     := open-elevator
FQBN        := teensy:avr:teensy41
BAUD        := 115200

# arduino-cli resolves ARDUINO_DIRECTORIES_* from the environment,
# set by the Nix shell hook. If you are running outside of nix develop,
# set _ARDUINO_PROJECT_DIR manually before invoking make.

CLI         := arduino-cli
PICOCOM     := picocom

# Auto-detect the Teensy serial port. Teensy 4.1 shows up as /dev/ttyACM*
# on Linux. Override PORT on the command line if you have multiple devices:
#   make upload PORT=/dev/ttyACM1
PORT        ?= $(firstword $(wildcard /dev/ttyACM*))

.PHONY: all compile upload monitor install clean help

all: compile

## install: fetch the Teensy core and all libraries declared in sketch.yaml
install:
	$(CLI) core update-index \
		--additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
	$(CLI) core install teensy:avr \
		--additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
	$(CLI) lib install "U8g2"

## compile: build the sketch against the Teensy 4.1 profile
compile:
	$(CLI) compile \
		--profile $(PROFILE) \
		--warnings all \
		$(SKETCH)

## upload: compile and flash. Requires the Teensy udev rules on Linux.
upload: compile
	@if [ -z "$(PORT)" ]; then \
		echo "No serial port found. Is the Teensy plugged in?"; \
		exit 1; \
	fi
	$(CLI) upload \
		--profile $(PROFILE) \
		--port $(PORT) \
		$(SKETCH)

## monitor: open a serial monitor at BAUD. Ctrl-A Ctrl-X to exit picocom.
monitor:
	@if [ -z "$(PORT)" ]; then \
		echo "No serial port found. Is the Teensy plugged in?"; \
		exit 1; \
	fi
	$(PICOCOM) --baud $(BAUD) $(PORT)

## flash: upload without recompiling (uses last build artifact)
flash:
	@if [ -z "$(PORT)" ]; then \
		echo "No serial port found. Is the Teensy plugged in?"; \
		exit 1; \
	fi
	$(CLI) upload \
		--fqbn $(FQBN) \
		--port $(PORT) \
		$(SKETCH)

## clean: remove build artifacts from the sketch directory
clean:
	rm -rf build/

## help: list available targets
help:
	@grep -E '^##' Makefile | sed 's/## /  /'