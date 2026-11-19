# Build NickelSync for firmware 4.x (Qt 5) with NickelTC:
#   git submodule update --init
#   podman run --rm -v "$PWD":/src -w /src ghcr.io/pgaskin/nickeltc:1.0 make clean all koboroot
# Build for firmware 6.x (Qt 6) in the kobuild Qt 6 image with NH_QT_MAJOR=6.
# tools/build-kobo.sh builds both and writes one Kobo.tgz for 4.x and 6.x.

NH_QT_MAJOR ?= 5

include ./NickelHook/NickelHook.mk

override LIBRARY  := $(if $(filter 6,$(NH_QT_MAJOR)),libnickelsync-qt6.so,libnickelsync.so)
override SOURCES  += src/nickelsync.cc src/settings.cc src/autosync.cc src/serverstats.cc src/hardcover.cc src/hardcoverpage.cc
override CFLAGS   += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override MOCS     += src/nickelsync.h src/settings.h src/autosync.h src/serverstats.h src/hardcover.h
override PKGCONF  += Qt$(NH_QT_MAJOR)Widgets Qt$(NH_QT_MAJOR)Network Qt$(NH_QT_MAJOR)Sql
override KOBOROOT += res/nm:/mnt/onboard/.adds/nm/nickelsync
override KOBOROOT += res/uninstall:/mnt/onboard/.adds/nickelsync/uninstall

include ./NickelHook/NickelHook.mk

# The source embeds res/nm with .incbin.
src/nickelsync.o: res/nm
