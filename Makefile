ARM_CC ?= arm-linux-gnueabi-gcc
HOST_CC ?= gcc
BUILD_DIR ?= build
TEST_TIMEOUT ?= 15
VERSION := $(strip $(shell cat VERSION))
VERSION_CPPFLAGS = -DR1_CONTROLLER_VERSION='"$(VERSION)"'
DEVICE ?= root@192.168.205.1
BOOK_MOUNT ?=
WARN = -std=gnu11 -Wall -Wextra -Wpedantic -Werror
TESTS = cursor-raster usb-state touch-click r1-event-queue pointer-motion foreground-handoff cursor-update-plan epdc-background-preserve cursor-restore-mode cursor-repair-plan cursor-repair-seed controller-map controller-launch launcher native-input

.PHONY: all test package check-renderer deploy-temporary install uninstall
all: $(BUILD_DIR)/r1-controller $(BUILD_DIR)/r1-launcher
$(BUILD_DIR)/r1-controller: src/r1-controller.c $(wildcard src/*.h) VERSION Makefile
	mkdir -p "$(BUILD_DIR)"
	$(ARM_CC) $(CPPFLAGS) $(VERSION_CPPFLAGS) -O2 $(WARN) -Wl,--export-dynamic $< -o $@ -ldl -lrt

$(BUILD_DIR)/r1-launcher: src/r1-launcher.c Makefile
	mkdir -p "$(BUILD_DIR)"
	$(ARM_CC) $(CPPFLAGS) -O2 $(WARN) $< -o $@

$(BUILD_DIR)/tests/%-test: tests/%-test.c src/r1-controller.c src/r1-launcher.c $(wildcard src/*.h) VERSION Makefile
	mkdir -p "$(BUILD_DIR)/tests"
	$(HOST_CC) $(CPPFLAGS) $(VERSION_CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-pie -no-pie $< -o $@ -ldl -lrt -pthread

check-renderer:
	sha256sum -c tests/renderer-baseline.sha256

test: check-renderer $(addprefix $(BUILD_DIR)/tests/,$(addsuffix -test,$(TESTS)))
	@set -e; for test in $(TESTS); do timeout $(TEST_TIMEOUT)s "$(BUILD_DIR)/tests/$$test-test"; done
	sh -n scripts/install-device.sh
	bash -n scripts/deploy.sh
	@set -e; for app in packaging/applications/*.app; do sh -n "$$app"; done
	timeout $(TEST_TIMEOUT)s bash tests/install-device-test.sh

package: all
	tar -czf "$(BUILD_DIR)/r1-controller-$(VERSION).tar.gz" --transform 's|^|r1-controller-$(VERSION)/|' -C "$(BUILD_DIR)" r1-controller r1-launcher -C "$(CURDIR)" README.md CHANGELOG.md VERSION docs/KEY_MAP.md docs/INSTALL.md scripts packaging
	timeout $(TEST_TIMEOUT)s bash tests/package-test.sh "$(BUILD_DIR)/r1-controller-$(VERSION).tar.gz"

deploy-temporary: $(BUILD_DIR)/r1-controller
	R1_BINARY_DIR="$(abspath $(BUILD_DIR))" bash scripts/deploy.sh temporary "$(DEVICE)"

install: all
	R1_BINARY_DIR="$(abspath $(BUILD_DIR))" bash scripts/deploy.sh install "$(DEVICE)" "$(BOOK_MOUNT)"

uninstall:
	bash scripts/deploy.sh uninstall "$(DEVICE)" "$(BOOK_MOUNT)"
