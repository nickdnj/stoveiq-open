# StoveIQ — Convenience Makefile
# Use PlatformIO for the real build. This Makefile is for quick commands.

.PHONY: build flash test emulator clean upload-fs

build:
	pio run

flash:
	pio run -t upload

test:
	pio test -e emulator

emulator:
	pio run -e emulator
	.pio/build/emulator/program

upload-fs:
	pio run -t uploadfs

clean:
	pio run -t clean

monitor:
	pio device monitor --baud 115200
