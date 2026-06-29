# Standalone build for labrador-cli.
#
# This compiles the Librador API sources directly into the CLI binary, so there
# is no separate shared-library or Qt dependency -- only a C++17 compiler and
# libusb-1.0 are required.
#
#   Linux/Raspberry Pi:  sudo apt install libusb-1.0-0-dev && make
#   macOS:               brew install libusb && make
#
# Build with logging:    make DEBUG=1
# Install:               sudo make install   (PREFIX defaults to /usr/local)

TARGET   := labrador-cli
LIBDIR   := ../librador
PREFIX   ?= /usr/local

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
CXXFLAGS += -I$(LIBDIR)

SRCS := labrador_cli.cpp \
        $(LIBDIR)/librador.cpp \
        $(LIBDIR)/usbcallhandler.cpp \
        $(LIBDIR)/o1buffer.cpp

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    CXXFLAGS += -DPLATFORM_MAC -I$(shell brew --prefix)/include/libusb-1.0
    LDFLAGS  += -L$(shell brew --prefix)/lib -lusb-1.0
else
    CXXFLAGS += -DPLATFORM_LINUX $(shell pkg-config --cflags libusb-1.0)
    LDFLAGS  += $(shell pkg-config --libs libusb-1.0) -lpthread
endif

ifdef DEBUG
    CXXFLAGS += -DLIBRADOR_ENABLE_LOGGING -g
endif

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
