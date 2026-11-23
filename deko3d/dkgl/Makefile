# libdkgl.a: OpenGL subset + EGL on deko3d (link with ../uam-switch/libuam.a and -ldeko3d -lnx)
ifeq ($(strip $(DEVKITPRO)),)
$(error Please set DEVKITPRO)
endif
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := libdkgl.a
SRCS     := dkgl_core.cpp dkgl_gl.cpp dkgl_shader.cpp
OBJS     := $(SRCS:%.cpp=build/%.o)
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS := -O2 -g -Wall -Wno-unused-function -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
            -std=gnu++17 -fno-exceptions -fno-rtti \
            -I$(DEVKITPRO)/libnx/include -I$(DEVKITPRO)/portlibs/switch/include -I../uam-switch

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo built ... $@

build/%.o: %.cpp dkgl_internal.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build $(TARGET)
