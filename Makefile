# Makefile — Collect Call B2BUA (PJSUA C API)
# Usage:
#   make              — build ./collect_call
#   make install      — install to /opt/collect_call
#   make clean        — remove build artefacts

CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE
TARGET   := collect_call
INSTALL_DIR := /opt/collect_call

# ── PJSIP ────────────────────────────────────────────────────────────────
PJSIP_CFLAGS := $(shell pkg-config --cflags libpjproject)

# --static pulls Libs.private (codec libs, OpenSSL, macOS frameworks).
PJSIP_LDFLAGS := $(shell pkg-config --libs --static libpjproject 2>/dev/null)
ifeq ($(shell uname -s),Darwin)
# Homebrew OpenSSL is keg-only; add its -L path for -lssl/-lcrypto above.
PJSIP_LDFLAGS += $(shell pkg-config --libs openssl 2>/dev/null)
endif

# Fallback for Linux builds where --static is unavailable.
ifeq ($(PJSIP_LDFLAGS),)
PJSIP_SUFFIX := $(shell pkg-config --libs libpjproject 2>/dev/null | sed -n 's/.*-lpj-\([^ ]*\).*/\1/p')
PJSIP_LDFLAGS := $(shell pkg-config --libs libpjproject) \
	-lilbccodec-$(PJSIP_SUFFIX) -lwebrtc-$(PJSIP_SUFFIX) \
	-lresample-$(PJSIP_SUFFIX) -lsrtp-$(PJSIP_SUFFIX) \
	-lgsmcodec-$(PJSIP_SUFFIX) -lspeex-$(PJSIP_SUFFIX) \
	-lssl -lcrypto -lasound -lm -lrt -lpthread -luuid
endif


INCLUDES := -Iinclude $(PJSIP_CFLAGS)

SRCS := src/main.c    \
        src/app_logger.c \
        src/b2bua.c   \
        src/env_loader.c \
        src/leg_a.c   \
        src/leg_b.c   \
        src/session.c \
	src/utils.c   \
	src/validation.c \
	src/api_mapping.c \
	src/runtime_config.c \
	src/prompt_mapping.c \
	src/options.c

OBJS := $(SRCS:src/%.c=build/%.o)

# ── Targets ───────────────────────────────────────────────────────────────

.PHONY: all build_dir install clean

all: build_dir $(TARGET)

build_dir:
	@mkdir -p build

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(PJSIP_LDFLAGS)
	@echo "Build complete: $(TARGET)"

build/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: $(TARGET)
	install -d $(INSTALL_DIR)/wav
	install -m 755 $(TARGET) $(INSTALL_DIR)/
	@if ls wav/*.wav 1>/dev/null 2>&1; then \
	    install -m 644 wav/*.wav $(INSTALL_DIR)/wav/; \
	    echo "WAV files installed."; \
	else \
	    echo "WARNING: No WAV files found — add them to $(INSTALL_DIR)/wav/"; \
	fi
	@if [ -f wav/wav_mapping.conf ]; then \
	    install -m 644 wav/wav_mapping.conf $(INSTALL_DIR)/wav/; \
	    echo "WAV mapping installed."; \
	fi
	@echo "Installed to $(INSTALL_DIR)"

clean:
	rm -rf build/ $(TARGET)
