# Builds UAM (deko3d shader compiler) as a static library for the Switch: libuam.a
# make -j8   (needs DEVKITPRO, generated sources in ../uam-gen)

ifeq ($(strip $(DEVKITPRO)),)
$(error Please set DEVKITPRO)
endif
include $(DEVKITPRO)/libnx/switch_rules

UAM     := ../uam
GEN     := ../uam-gen
BUILD   := build
TARGET  := libuam.a

UAM_SRC := $(addprefix $(UAM)/,$(filter-out source/main.cpp,$(shell cat ../uam-sources.txt)))
GEN_SRC := $(GEN)/glsl/glsl_parser.cpp $(GEN)/glsl/glsl_lexer.cpp \
           $(GEN)/glsl/glcpp/glcpp-parse.c $(GEN)/glsl/glcpp/glcpp-lex.c
SRCS    := $(UAM_SRC) $(GEN_SRC) uam_lib.cpp

OBJS    := $(patsubst %,$(BUILD)/%.o,$(subst ../,,$(SRCS)))

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFS    := -D__SWITCH__ -DPACKAGE_STRING='"uam 1.1.0-cne"' -DDESKTOP -D_USE_MATH_DEFINES -D_GNU_SOURCE \
           -DHAVE_POSIX_MEMALIGN -DNDEBUG
INCS    := -I$(UAM)/mesa-imported -I$(UAM)/source -I$(UAM)/mesa-imported/glsl -I$(UAM)/mesa-imported/glsl/glcpp \
           -I$(GEN) -I$(GEN)/glsl -I$(GEN)/glsl/glcpp -I$(DEVKITPRO)/libnx/include -I.
COMMON  := -O2 -g0 -ffunction-sections -fdata-sections $(ARCH) $(DEFS) $(INCS) -w
CFLAGS  := $(COMMON) -std=gnu99
CXXFLAGS:= $(COMMON) -std=gnu++11 -fno-exceptions -Wno-class-memaccess

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo built ... $@

$(BUILD)/%.c.o: ../%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: ../%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET)
