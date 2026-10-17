PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

TARGET := FIROZ_OS.elf
SRCS   := main.c
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter
LDADD  := -lpthread

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDADD)

clean:
	rm -f $(TARGET)

.PHONY: all clean
