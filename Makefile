#---------------------------------------------------------------------------------
# Konnect3DS - Nintendo 3DS homebrew save-to-cloud backup tool
#
# Requires devkitARM + libctru (devkitPro pacman package `3ds-dev`).
# export DEVKITPRO=/opt/devkitpro ; export DEVKITARM=$DEVKITPRO/devkitARM
#
# `make`      -> Konnect3DS.3dsx (run via Homebrew Launcher, no extra tools)
# `make cia`  -> Konnect3DS.cia (install with FBI). Needs makerom + bannertool,
#                which come from devkitPro's `general-tools` pacman package:
#                  (dkp-)pacman -S general-tools
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS
# RESOURCES holds the CIA-only metadata: AppInfo (title/author/ids) and
#   template.rsf (permissions), plus icon.png/banner.png/audio.wav
#---------------------------------------------------------------------------------
TARGET		:=	Konnect3DS
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs
RESOURCES	:=	resources

include $(TOPDIR)/$(RESOURCES)/AppInfo

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-fomit-frame-pointer -ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	-lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring icon.png,$(icons)))
		export APP_ICON := $(TOPDIR)/icon.png
	else
		export APP_ICON := $(TOPDIR)/$(RESOURCES)/icon.png
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile 3dsx

cia: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

$(BUILD):
	@[ -d $@ ] || mkdir -p $@

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia icon.icn banner.bnr

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

APP_TITLE         := $(shell echo "$(APP_TITLE)" | cut -c1-128)
APP_DESCRIPTION   := $(shell echo "$(APP_DESCRIPTION)" | cut -c1-256)
APP_AUTHOR        := $(shell echo "$(APP_AUTHOR)" | cut -c1-128)
APP_PRODUCT_CODE  := $(shell echo $(APP_PRODUCT_CODE) | cut -c1-16)
APP_UNIQUE_ID     := $(shell echo $(APP_UNIQUE_ID) | cut -c1-7)
APP_VERSION_MAJOR := $(shell echo $(APP_VERSION_MAJOR) | cut -c1-3)
APP_VERSION_MINOR := $(shell echo $(APP_VERSION_MINOR) | cut -c1-3)
APP_VERSION_MICRO := $(shell echo $(APP_VERSION_MICRO) | cut -c1-3)
APP_ROMFS         := $(TOPDIR)/$(ROMFS)
APP_SYSTEM_MODE   := 64MB
APP_SYSTEM_MODE_EXT := Legacy

RSF          := $(TOPDIR)/$(RESOURCES)/template.rsf
BANNER_IMAGE := $(TOPDIR)/$(RESOURCES)/banner.png
BANNER_AUDIO := $(TOPDIR)/$(RESOURCES)/audio.wav

COMMON_MAKEROM_PARAMS := -rsf $(RSF) -target t -exefslogo -elf $(OUTPUT).elf -icon icon.icn \
-banner banner.bnr -DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
-DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)" -DAPP_ROMFS="$(APP_ROMFS)" -DAPP_SYSTEM_MODE="$(APP_SYSTEM_MODE)" \
-DAPP_SYSTEM_MODE_EXT="$(APP_SYSTEM_MODE_EXT)" -major "$(APP_VERSION_MAJOR)" -minor "$(APP_VERSION_MINOR)" \
-micro "$(APP_VERSION_MICRO)"

ifeq ($(OS),Windows_NT)
	MAKEROM := makerom.exe
	BANNERTOOL := bannertool.exe
else
	MAKEROM := makerom
	BANNERTOOL := bannertool
endif

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
.PHONY: 3dsx cia

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

3dsx: $(OUTPUT).3dsx

icon.icn: $(APP_ICON)
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_TITLE)" -p "$(APP_AUTHOR)" -i $(APP_ICON) -o icon.icn
	@echo built ... $(notdir $@)

banner.bnr: $(BANNER_IMAGE) $(BANNER_AUDIO)
	@$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o banner.bnr
	@echo built ... $(notdir $@)

$(OUTPUT).cia: $(OUTPUT).elf banner.bnr icon.icn
	@$(MAKEROM) -f cia -o $(OUTPUT).cia -DAPP_ENCRYPTED=false $(COMMON_MAKEROM_PARAMS)
	@echo built ... $(notdir $@)

cia: $(OUTPUT).cia

#---------------------------------------------------------------------------------
%_bin.h %.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
