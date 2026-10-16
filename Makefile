# ============================================================================
# FIROZ OS Payload - Makefile with Custom Linker Script (v4.0 Optimized)
# ============================================================================

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

TARGET := FIROZ_OS.elf
SRCS   := main.c
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter

# استفاده از اسکریپت لینکر اختصاصی برای تفکیک دقیق سگمنت‌ها (جلوگیری از خطای RWX)
LDFLAGS := -Wl,--no-dynamic-linker -Wl,-T,linker.ld -Wl,--as-needed

# کتابخانه‌های استاندارد کرنل و تردینگ
LDADD := -lkernel -lpthread

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld
	@echo "[*] Compiling $(TARGET) with custom linker script..."
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDADD)
	@echo "[+] Build successful: $(TARGET)"
	@echo "[*] Verifying segment flags..."
	@readelf -l $(TARGET) | grep -A 1 "Type\|Flags" || true

clean:
	@echo "[*] Cleaning build artifacts..."
	rm -f $(TARGET)
	@echo "[+] Clean complete"

.PHONY: all clean
