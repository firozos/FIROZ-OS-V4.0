# ============================================================================
# FIROZ OS v4.0 - Ultimate Makefile for PS5 Payload (PIE Enabled)
# ============================================================================

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

TARGET := FIROZ_OS.elf
SRCS   := main.c

# تنظیمات بهینه‌سازی و تولید کد مستقل از موقعیت (PIE) برای سازگاری با لودر PS5
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -fPIC
LDFLAGS := -Wl,-pie
LDADD  := -lpthread

all: $(TARGET)

$(TARGET): $(SRCS)
	@echo "[*] Compiling $(TARGET) as PIE binary..."
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDADD)
	@echo "[+] Build successful: $(TARGET)"

clean:
	@echo "[*] Cleaning build artifacts..."
	rm -f $(TARGET)
	@echo "[+] Clean complete"

.PHONY: all clean
