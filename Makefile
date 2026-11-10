# Build NickelSync with NickelTC:
#   git submodule update --init
#   podman run --rm -v "$PWD":/src -w /src ghcr.io/pgaskin/nickeltc:1.0 make clean all koboroot

include ./NickelHook/NickelHook.mk

override LIBRARY  := libnickelsync.so
override SOURCES  += src/nickelsync.cc
override CFLAGS   += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override MOCS     += src/nickelsync.h
override KOBOROOT += res/nm:/mnt/onboard/.adds/nm/nickelsync
override KOBOROOT += res/uninstall:/mnt/onboard/.adds/nickelsync/uninstall

include ./NickelHook/NickelHook.mk
