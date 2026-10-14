# SlideNX Reborn: foreground Nintendo Switch homebrew app (.nro)
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO to your devkitPro installation")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules
TARGET := slidenx
BUILD := build
SOURCES := source
DATA :=
INCLUDES := include
NO_ICON := 1
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS += $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm
LIBDIRS := $(PORTLIBS) $(LIBNX)
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_SRC)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_TITLE := SlideNX Reborn
export APP_AUTHOR := Angelcraft
export APP_VERSION := 2.4.0
.PHONY: all clean
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile
clean:
	@echo clean ...
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).map
else
DEPENDS := $(OFILES:.o=.d)
.PHONY: all
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
$(OFILES_SRC): $(HFILES_BIN)
-include $(DEPENDS)
endif
