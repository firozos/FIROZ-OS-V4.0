# ============================================================================
# FIROZ OS v4.0 - Makefile (PS5 Jailbreak Payload Compiler)
# ============================================================================

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

TARGET := FIROZ_OS.elf
SRCS   := main.c

# بهینه‌سازی و هشدارها
CFLAGS += -O2 -Wall -Wextra -Wno-unused-parameter

# کتابخانه‌های مورد نیاز برای تردینگ و شبکه
LDADD  += -lpthread

all: $(TARGET)

$(TARGET): $(SRCS)
	@echo "[*] Compiling $(TARGET)..."
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDADD)
	@echo "[+] Build successful: $(TARGET)"

clean:
	@echo "[*] Cleaning build artifacts..."
	rm -f $(TARGET)
	@echo "[+] Clean complete"

.PHONY: all clean
