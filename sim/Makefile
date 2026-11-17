# NickelSync desktop sim. Builds src/nickelsync.cc unmodified against desktop
# Qt 5 (WSL Ubuntu: qtbase5-dev qtbase5-dev-tools), with the Nickel stand-ins in
# nickelstub.cc. The device build (../Makefile) does not use this file.
#
# Not -Werror: desktop Qt 5.15 has deprecation warnings that Qt 5.2 does not.

BUILD     := build
CXX       := g++
MOC       := moc
RCC       := rcc
QT_CFLAGS := $(shell pkg-config --cflags Qt5Widgets Qt5Network)
QT_LIBS   := $(shell pkg-config --libs Qt5Widgets Qt5Network)
CXXFLAGS  := -std=gnu++11 -fPIC -g $(QT_CFLAGS) -Wall -Wextra -Wno-deprecated-declarations \
             -Wno-missing-field-initializers -MMD -MP -I. -I../src -I../NickelHook

vpath %.cc ../src .
vpath %.h  ../src .

SRCS   := nickelsync.cc settings.cc autosync.cc serverstats.cc nickelstub.cc fakeserver.cc nh_sim.cc main.cc
MOCS   := nickelsync.h settings.h autosync.h serverstats.h nickelstub.h
OBJS   := $(addprefix $(BUILD)/,$(SRCS:.cc=.o)) $(addprefix $(BUILD)/moc_,$(MOCS:.h=.o)) $(BUILD)/qrc_sim.o
TARGET := nickelsync-sim

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(QT_LIBS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.cc | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/moc_%.cpp: %.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/moc_%.o: $(BUILD)/moc_%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/qrc_sim.cpp: res/sim.qrc res/sync.svg res/sync_hit.svg res/hardcover.svg | $(BUILD)
	$(RCC) -name sim $< -o $@

$(BUILD)/qrc_sim.o: $(BUILD)/qrc_sim.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET)

-include $(wildcard $(BUILD)/*.d)
