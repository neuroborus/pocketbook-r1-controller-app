ARM_CC ?= arm-linux-gnueabi-gcc
HOST_CC ?= gcc
TEST_TIMEOUT ?= 15
VERSION := $(strip $(shell cat VERSION))
VERSION_CPPFLAGS = -DR1_CONTROLLER_VERSION='"$(VERSION)"'
WARN = -std=gnu11 -Wall -Wextra -Wpedantic -Werror
TESTS = cursor-raster usb-state touch-click r1-event-queue pointer-motion foreground-handoff cursor-update-plan epdc-background-preserve cursor-restore-mode cursor-repair-plan cursor-repair-seed controller-map controller-launch

.PHONY: all test package check-renderer
all: build/r1-controller
build/r1-controller: src/r1-controller.c $(wildcard src/*.h) VERSION Makefile
	mkdir -p build
	$(ARM_CC) $(CPPFLAGS) $(VERSION_CPPFLAGS) -O2 $(WARN) -Wl,--export-dynamic $< -o $@ -ldl

build/tests/%-test: tests/%-test.c src/r1-controller.c $(wildcard src/*.h) VERSION Makefile
	mkdir -p build/tests
	$(HOST_CC) $(CPPFLAGS) $(VERSION_CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-pie -no-pie $< -o $@ -ldl -pthread

check-renderer:
	sha256sum -c tests/renderer-baseline.sha256

test: check-renderer $(addprefix build/tests/,$(addsuffix -test,$(TESTS)))
	@set -e; for test in $(TESTS); do timeout $(TEST_TIMEOUT)s build/tests/$$test-test; done

package: all
	tar -czf build/r1-controller-$(VERSION).tar.gz --transform 's|^|r1-controller-$(VERSION)/|' -C build r1-controller -C .. README.md CHANGELOG.md VERSION docs/KEY_MAP.md
