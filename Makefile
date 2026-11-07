# buddy3d-local: local-only firmware overlay for the Prusa Buddy3D camera (RV1103)
#
#   make            fetch toolchain/SDK (first time), build everything, assemble out/sdcard/
#   make sdcard     same as above
#   make camd       build just the camera daemon
#   make deploy     copy out/sdcard/ to a mounted SD card:  make deploy SD=/media/sd
#   make dist       release archives (zip + tar.gz) with the example config only
#   make clean      remove build outputs (keeps downloads)
#   make distclean  also remove downloaded toolchain/SDK/sources
#
# Set LUCKFOX_SDK=/path/to/luckfox-pico to reuse an existing SDK checkout.

SHELL := /bin/bash

DEPS    := deps
BUILD   := build
OUT     := out/sdcard

# Luckfox Pico SDK subset (toolchain, Rockit, rkaiq, librtsp), pinned
LUCKFOX_COMMIT := 824b817f889c2cbff1d48fcdb18ab494a68f69d1
SDK      := $(DEPS)/luckfox-pico
TC       := $(SDK)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
CROSS    := $(abspath $(TC))/bin/arm-rockchip830-linux-uclibcgnueabihf-
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip
HOST     := arm-rockchip830-linux-uclibcgnueabihf

ROCKIT   := $(SDK)/media/rockit/rockit
AIQ      := $(SDK)/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf
MISC     := $(SDK)/media/common_algorithm/common_algorithm/misc

SDK_STAMP := $(SDK)/.fetched

# ---- dropbear (ssh) -------------------------------------------------------
DROPBEAR_VER    := 2026.94
DROPBEAR_URL    := https://matt.ucc.asn.au/dropbear/releases/dropbear-$(DROPBEAR_VER).tar.bz2
DROPBEAR_SHA256 := e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
DROPBEAR_SRC    := $(DEPS)/dropbear-$(DROPBEAR_VER)
DROPBEAR_BIN    := $(BUILD)/dropbear/dropbearmulti

# ---- Spleen bitmap font (OSD), BSD-2-Clause --------------------------------
SPLEEN_VER    := 2.2.0
SPLEEN_URL    := https://github.com/fcambus/spleen/releases/download/$(SPLEEN_VER)/spleen-$(SPLEEN_VER).tar.gz
# upstream publishes no checksum; this pins the release tarball as first fetched
SPLEEN_SHA256 := ec42925c6b56d2138c862b2f97147c872e472f674bf03423417d827a08d69a89
FONT_H        := $(BUILD)/font/font_spleen.h

# ---- camd -----------------------------------------------------------------
CAMD_SRCS := $(wildcard src/camd/*.c)
CAMD_HDRS := $(wildcard src/camd/*.h)
CAMD_BIN  := $(BUILD)/camd

AIQ_INC := $(AIQ)/include/rkaiq
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-march=armv7-a -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections \
	-DISP_HW_V32 \
	-I$(ROCKIT)/mpi/sdk/include -I$(MISC)/include -I$(BUILD)/font \
	-I$(AIQ_INC) -I$(AIQ_INC)/uAPI2 -I$(AIQ_INC)/common -I$(AIQ_INC)/xcore \
	-I$(AIQ_INC)/algos -I$(AIQ_INC)/iq_parser -I$(AIQ_INC)/iq_parser_v2
# Link against the SDK's libs, but at runtime use the camera's own copies in
# /oem/usr/lib (they must match the camera's kernel modules).
LDFLAGS := -Wl,--gc-sections -Wl,--allow-shlib-undefined \
	-Wl,-rpath,/oem/usr/lib:/usr/lib \
	-L$(ROCKIT)/lib/lib32 -L$(AIQ)/lib -L$(MISC)/lib/$(HOST)
LDLIBS := -Wl,-Bstatic -lrtsp -Wl,-Bdynamic -lrockit -lrkaiq -lpthread -lm

.PHONY: all sdcard camd dropbear deps deploy push dist clean distclean

all: sdcard

deps: $(SDK_STAMP)

$(SDK_STAMP):
	@mkdir -p $(DEPS)
	scripts/fetch-sdk.sh $(SDK) $(LUCKFOX_COMMIT)
	@touch $@

camd: $(CAMD_BIN)

$(DEPS)/spleen-$(SPLEEN_VER).tar.gz:
	@mkdir -p $(DEPS)
	curl -fL -o $@.tmp $(SPLEEN_URL)
	echo "$(SPLEEN_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(FONT_H): $(DEPS)/spleen-$(SPLEEN_VER).tar.gz tools/bdf2c.py
	@mkdir -p $(BUILD)/font
	tar -C $(BUILD)/font -xzf $< spleen-$(SPLEEN_VER)/spleen-8x16.bdf \
	  spleen-$(SPLEEN_VER)/spleen-16x32.bdf spleen-$(SPLEEN_VER)/LICENSE
	{ python3 tools/bdf2c.py $(BUILD)/font/spleen-$(SPLEEN_VER)/spleen-8x16.bdf FONT16; \
	  python3 tools/bdf2c.py $(BUILD)/font/spleen-$(SPLEEN_VER)/spleen-16x32.bdf FONT32; } > $@.tmp
	mv $@.tmp $@

$(CAMD_BIN): $(CAMD_SRCS) $(CAMD_HDRS) $(FONT_H) | $(SDK_STAMP)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CAMD_SRCS) $(LDFLAGS) $(LDLIBS)
	$(STRIP) $@

# ---- dropbear --------------------------------------------------------------
dropbear: $(DROPBEAR_BIN)

$(DEPS)/dropbear-$(DROPBEAR_VER).tar.bz2:
	@mkdir -p $(DEPS)
	curl -fL -o $@.tmp $(DROPBEAR_URL)
	echo "$(DROPBEAR_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(DROPBEAR_SRC)/.unpacked: $(DEPS)/dropbear-$(DROPBEAR_VER).tar.bz2
	tar -C $(DEPS) -xjf $<
	@touch $@

# dropbear picks up ./localoptions.h from the (out-of-tree) build directory
$(DROPBEAR_BIN): $(DROPBEAR_SRC)/.unpacked src/dropbear/localoptions.h | $(SDK_STAMP)
	mkdir -p $(BUILD)/dropbear
	cp src/dropbear/localoptions.h $(BUILD)/dropbear/localoptions.h
	cd $(BUILD)/dropbear && \
	  [ -f Makefile ] || $(abspath $(DROPBEAR_SRC))/configure --host=$(HOST) \
	    CC=$(CC) --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx \
	    --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline \
	    --disable-harden >/dev/null
	$(MAKE) -C $(BUILD)/dropbear PROGRAMS="dropbear dropbearkey scp" MULTI=1 \
	  -j$$(nproc) >/dev/null
	$(STRIP) $@

# ---- SD card image directory ----------------------------------------------
# Layout on the card:
#   lp_app.sh            boot hook (the camera's init runs this instead of the stock app)
#   camera.conf          your settings
#   buddy3d/bin/         camd, dropbearmulti
#   buddy3d/authorized_keys, ssh_host_ed25519_key, logs/   (created/used at runtime)
#
# Personal files are taken from sdcard/camera.conf and sdcard/authorized_keys if
# present (both git-ignored), or AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub.
AUTHORIZED_KEYS ?= $(wildcard sdcard/authorized_keys)

sdcard: $(CAMD_BIN) $(DROPBEAR_BIN) $(wildcard sdcard/*)
	rm -rf $(OUT)
	mkdir -p $(OUT)/buddy3d/bin
	cp sdcard/lp_app.sh $(OUT)/lp_app.sh
	cp sdcard/camera.conf.example $(OUT)/buddy3d/camera.conf.example
	if [ -f sdcard/camera.conf ]; then cp sdcard/camera.conf $(OUT)/camera.conf; \
	  else cp sdcard/camera.conf.example $(OUT)/camera.conf; fi
	if [ -n "$(AUTHORIZED_KEYS)" ]; then cat $(AUTHORIZED_KEYS) > $(OUT)/buddy3d/authorized_keys; fi
	cp $(CAMD_BIN) $(DROPBEAR_BIN) $(OUT)/buddy3d/bin/
	cp $(BUILD)/font/spleen-$(SPLEEN_VER)/LICENSE $(OUT)/buddy3d/LICENSE.spleen-font
	@echo
	@echo "SD card contents ready in $(OUT)/ - copy them to the root of a FAT32 card"
	@echo "(or: make deploy SD=/mount/point) and edit camera.conf (Wi-Fi etc.)."
	@du -sh $(OUT)

# Copy to a mounted card. Keeps an existing camera.conf, host key and logs.
deploy: sdcard
	@[ -n "$(SD)" ] || { echo "usage: make deploy SD=/path/to/mounted/sdcard"; exit 1; }
	mkdir -p $(SD)/buddy3d
	cp $(OUT)/lp_app.sh $(SD)/
	[ -f $(SD)/camera.conf ] || cp $(OUT)/camera.conf $(SD)/
	cp -r $(OUT)/buddy3d/. $(SD)/buddy3d/
	sync

# Development: push freshly built binaries to a running camera over telnet and
# restart camd (CAM=<camera address>, default buddy3d).
push: $(CAMD_BIN)
	tools/cam.py run 'touch /tmp/buddy3d/camd.pause; killall camd; sleep 1; killall -9 camd 2>/dev/null; true'
	tools/cam.py push $(CAMD_BIN) /tmp/buddy3d/bin/camd
	tools/cam.py run 'rm -f /tmp/buddy3d/camd.pause'

# ---- release archives -----------------------------------------------------
# Built from the example config only: never includes sdcard/camera.conf or
# sdcard/authorized_keys.
VERSION   ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
DIST_NAME := buddy3d-local-$(VERSION)
DIST_DIR  := out/dist/$(DIST_NAME)

dist: $(CAMD_BIN) $(DROPBEAR_BIN) $(FONT_H)
	rm -rf out/dist
	mkdir -p $(DIST_DIR)/buddy3d/bin
	cp sdcard/lp_app.sh $(DIST_DIR)/
	cp sdcard/camera.conf.example $(DIST_DIR)/camera.conf
	cp sdcard/camera.conf.example $(DIST_DIR)/buddy3d/camera.conf.example
	cp $(CAMD_BIN) $(DROPBEAR_BIN) $(DIST_DIR)/buddy3d/bin/
	cp $(BUILD)/font/spleen-$(SPLEEN_VER)/LICENSE $(DIST_DIR)/buddy3d/LICENSE.spleen-font
	cp README.md $(DIST_DIR)/buddy3d/README.md
	cp LICENSE $(DIST_DIR)/buddy3d/LICENSE
	echo "$(VERSION)" > $(DIST_DIR)/buddy3d/VERSION
	# archives hold the card contents at their root: extract straight onto the card
	cd $(DIST_DIR) && zip -qr ../$(DIST_NAME).zip . && tar -czf ../$(DIST_NAME).tar.gz .
	@ls -l out/dist/$(DIST_NAME).*

# ---- CI helpers -------------------------------------------------------------
# Cache keys: each external dependency is keyed on its pinned version, so a
# cache only changes when that dependency is bumped.
SDK_CACHE_KEY      := luckfox-sdk-$(LUCKFOX_COMMIT)
DROPBEAR_CACHE_KEY := dropbear-$(DROPBEAR_VER)-$(DROPBEAR_SHA256)
SPLEEN_CACHE_KEY   := spleen-$(SPLEEN_VER)-$(SPLEEN_SHA256)

print-%:
	@echo '$($*)'

clean:
	rm -rf $(BUILD) out

distclean: clean
	rm -rf $(DEPS)
