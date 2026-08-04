# Victoria Sims build.
#
#   make linux    native OpenGL ES 2.0 executable (default)
#   make web      freestanding wasm32 module plus its host page
#   make armv5    ARMv5TE cross build of the portable engine core (EABI)
#   make oabi     the same, built as genuine ARM OABI
#   make armv7    Cortex-A8 with NEON, the reference handheld
#   make verify   run the rasterizer checks on the host
#   make check    static enforcement of the no-dynamic-allocation rule
#   make clean
#
# Every toolchain choice below is overridable from the command line, because
# the targets we care about outlive whatever is installed today.

HOST_COMPILER ?= clang
WEB_COMPILER ?= clang
ARM_COMPILER ?= arm-linux-gnueabi-gcc
ARCHIVER ?= ar

BUILD_DIRECTORY ?= build
INCLUDE_FLAGS := -Iengine/include -I.

WARNING_FLAGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
                 -Wpointer-arith -Wcast-qual -Wwrite-strings
STANDARD_FLAGS := -std=c99 -pedantic
OPTIMIZATION_FLAGS ?= -O2
EXTRA_FLAGS ?=

COMMON_FLAGS := $(STANDARD_FLAGS) $(WARNING_FLAGS) $(OPTIMIZATION_FLAGS) $(INCLUDE_FLAGS) $(EXTRA_FLAGS)

ENGINE_SOURCES := engine/source/memoryArena.c \
                  engine/source/memoryBudget.c \
                  engine/source/freestandingRuntime.c \
                  engine/source/profiler.c \
                  engine/source/graphicsMemoryBudget.c \
                  engine/source/packageReader.c \
                  engine/source/engineCore.c \
                  utils/strings.c

# Which renderer the native build uses. openGLES2 needs a driver with
# programmable shaders; software needs nothing but a framebuffer, which is the
# only option on hardware of the Intel 2700G / NVIDIA GoForce generation. The
# choice is made here rather than at run time so a software build does not link
# libEGL and libGLESv2 at all.
RENDER_BACKEND ?= openGLES2

ifeq ($(RENDER_BACKEND),software)
LINUX_RENDER_SOURCES := render/software/renderSoftware.c \
                        render/software/rasterizer.c \
                        render/software/rasterizerNEON.c \
                        platform/linux/linuxPresenterSoftware.c
LINUX_LIBRARIES := -lX11
else
LINUX_RENDER_SOURCES := render/openGLES2/renderOpenGLES2.c \
                        platform/linux/linuxPresenterEGL.c
LINUX_LIBRARIES := -lEGL -lGLESv2 -lX11
endif

LINUX_SOURCES := $(ENGINE_SOURCES) \
                 $(LINUX_RENDER_SOURCES) \
                 platform/linux/linuxEntryPoint.c

WEB_SOURCES := $(ENGINE_SOURCES) \
               render/webGPU/renderWebGPU.c \
               platform/web/webEntryPoint.c

LINUX_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/linux
WEB_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/web
ARM_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/armv5

LINUX_EXECUTABLE := $(LINUX_OUTPUT_DIRECTORY)/victoriaSims
WEB_MODULE := $(WEB_OUTPUT_DIRECTORY)/victoriaSims.wasm
ARM_LIBRARY := $(ARM_OUTPUT_DIRECTORY)/libVictoriaEngine.a


# The wasm module owns exactly the reserved budget plus a fixed slack for the
# shadow stack and static data. Both bounds are pinned so the module can never
# grow past the ceiling at run time.
WEB_LINEAR_MEMORY_BYTES ?= 135266304

WEB_EXPORTS := -Wl,--export=victoriaWebInitialize \
               -Wl,--export=victoriaWebResize \
               -Wl,--export=victoriaWebRenderFrame \
               -Wl,--export=victoriaWebShutdown \
               -Wl,--export=victoriaWebGetBudgetTotalBytes \
               -Wl,--export=victoriaWebGetBudgetUsedBytes \
               -Wl,--export=victoriaWebGetProfilerReportPointer \
               -Wl,--export=victoriaWebGetProfilerReportLength \
               -Wl,--export=victoriaWebGetFrameMicroseconds \
               -Wl,--export=victoriaWebGetAverageFrameMicroseconds \
               -Wl,--export=victoriaWebGetWorstFrameMicroseconds \
               -Wl,--export=victoriaWebGetFrameIntervalMicroseconds \
               -Wl,--export=victoriaWebGetGraphicsMemoryLimitBytes \
               -Wl,--export=victoriaWebGetGraphicsMemoryUsedBytes

WEB_FLAGS := --target=wasm32 -ffreestanding -nostdlib -fno-builtin \
             -DVICTORIA_FREESTANDING_BUILTINS=1
WEB_LINK_FLAGS := -Wl,--no-entry -Wl,--allow-undefined \
                  -Wl,--initial-memory=$(WEB_LINEAR_MEMORY_BYTES) \
                  -Wl,--max-memory=$(WEB_LINEAR_MEMORY_BYTES) \
                  $(WEB_EXPORTS)

# --- ARM tiers ---------------------------------------------------------
# Three of them, oldest first. All are compile-only: there are no
# cross-compiled EGL or X11 libraries here, and the point is to prove the
# portable core stays portable.
#
#   armv5  the floor: ARMv5TE, no floating point unit assumed, soft-float ABI.
#          Covers the PXA25x/27x handhelds, whose graphics hardware predates
#          programmable shaders and so runs the software renderer.
#   oabi   the same code as genuine old-ABI, for pre-EABI userlands.
#   armv7  the reference device: Sharp NetWalker PC-Z1, i.MX515 Cortex-A8.

# The ARM tiers build the software renderer too: it is the backend the older
# devices in the ladder will actually run, so compiling only the core would
# leave the interesting half unchecked.
ARM_LIBRARY_SOURCES := $(ENGINE_SOURCES) \
                       render/software/renderSoftware.c \
                       render/software/rasterizer.c \
                       render/software/rasterizerNEON.c

ARM_ARCHITECTURE ?= armv5te
ARM_FLAGS := -march=$(ARM_ARCHITECTURE) -mfloat-abi=soft -ffreestanding \
             -DVICTORIA_FREESTANDING_BUILTINS=1

# Genuine old-ABI ARM. -mabi=apcs-gnu is a GCC ARM-backend codegen flag rather
# than a property of one particular toolchain build, so a stock
# arm-linux-gnueabi- cross compiler produces it too. Never assume it worked:
# the check target verifies ELF flags 0x600 (EABI version 0), because an
# accidentally-EABI binary looks perfectly fine right up until the device
# refuses to run it.
OABI_FLAGS := -mabi=apcs-gnu $(ARM_FLAGS)
OABI_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/oabi
OABI_LIBRARY := $(OABI_OUTPUT_DIRECTORY)/libVictoriaEngine.a
OABI_COMPILER ?= $(ARM_COMPILER)
OABI_READELF ?= arm-linux-gnueabi-readelf

# Cortex-A8 with VFPv3 and NEON. softfp rather than hard float on purpose: the
# NetWalker shipped Ubuntu 9.04, which is armel and predates armhf entirely.
# softfp gives hardware floating point instructions while keeping the
# soft-float calling convention that userland expects.
ARMV7_COMPILER ?= $(ARM_COMPILER)
ARMV7_ARCHITECTURE ?= armv7-a
ARMV7_TUNE ?= cortex-a8
ARMV7_FLAGS := -march=$(ARMV7_ARCHITECTURE) -mtune=$(ARMV7_TUNE) \
               -mfpu=neon -mfloat-abi=softfp -ffreestanding \
               -DVICTORIA_FREESTANDING_BUILTINS=1 -DVICTORIA_HAS_NEON=1
ARMV7_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/armv7
ARMV7_LIBRARY := $(ARMV7_OUTPUT_DIRECTORY)/libVictoriaEngine.a
ARMV7_READELF ?= arm-linux-gnueabi-readelf

# Compiles the portable core into one static library.
#   $(1) compiler   $(2) output directory   $(3) flags   $(4) library
define buildEngineLibrary
@mkdir -p $(2)
@for source in $(ARM_LIBRARY_SOURCES); do \
	objectName=$(2)/`basename $$source .c`.o; \
	echo "$(1) -c $$source -o $$objectName"; \
	$(1) $(COMMON_FLAGS) $(3) -c $$source -o $$objectName || exit 1; \
done
$(ARCHIVER) rcs $(4) $(2)/*.o
endef

.PHONY: all linux web armv5 armv7 oabi verify check clean

all: linux

linux: $(LINUX_EXECUTABLE)

$(LINUX_EXECUTABLE): $(LINUX_SOURCES)
	@mkdir -p $(LINUX_OUTPUT_DIRECTORY)
	$(HOST_COMPILER) $(COMMON_FLAGS) $(LINUX_SOURCES) $(LINUX_LIBRARIES) -o $@

web: $(WEB_MODULE)

$(WEB_MODULE): $(WEB_SOURCES) platform/web/index.html platform/web/victoriaRuntime.js
	@mkdir -p $(WEB_OUTPUT_DIRECTORY)
	$(WEB_COMPILER) $(COMMON_FLAGS) $(WEB_FLAGS) $(WEB_LINK_FLAGS) $(WEB_SOURCES) -o $@
	cp platform/web/index.html platform/web/victoriaRuntime.js $(WEB_OUTPUT_DIRECTORY)/

armv5: $(ARM_LIBRARY)

$(ARM_LIBRARY): $(ARM_LIBRARY_SOURCES)
	$(call buildEngineLibrary,$(ARM_COMPILER),$(ARM_OUTPUT_DIRECTORY),$(ARM_FLAGS),$@)

armv7: $(ARMV7_LIBRARY)

$(ARMV7_LIBRARY): $(ARM_LIBRARY_SOURCES)
	$(call buildEngineLibrary,$(ARMV7_COMPILER),$(ARMV7_OUTPUT_DIRECTORY),$(ARMV7_FLAGS),$@)
	@attributes=`$(ARMV7_READELF) -A $(ARMV7_OUTPUT_DIRECTORY)/memoryArena.o`; \
	echo "$$attributes" | grep -q 'Tag_CPU_arch: v7' || \
		{ echo "ERROR: not ARMv7" >&2; exit 1; }; \
	echo "$$attributes" | grep -q 'Tag_Advanced_SIMD_arch: NEON' || \
		{ echo "ERROR: NEON not enabled" >&2; exit 1; }; \
	echo "verified ARMv7-A with NEON"

oabi: $(OABI_LIBRARY)

$(OABI_LIBRARY): $(ARM_LIBRARY_SOURCES)
	$(call buildEngineLibrary,$(OABI_COMPILER),$(OABI_OUTPUT_DIRECTORY),$(OABI_FLAGS),$@)
	@flags=`$(OABI_READELF) -h $(OABI_OUTPUT_DIRECTORY)/memoryArena.o | grep -i '^ *Flags:'`; \
	case "$$flags" in \
		*0x600*) echo "verified genuine OABI ($$flags)" ;; \
		*) echo "ERROR: not OABI, got $$flags" >&2; exit 1 ;; \
	esac

RASTERIZER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyRasterizer
PACKAGE_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyPackageReader
DISC_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyDiscReader

verify: $(RASTERIZER_VERIFIER) $(PACKAGE_READER_VERIFIER) $(DISC_READER_VERIFIER)
	@$(RASTERIZER_VERIFIER)
	@$(PACKAGE_READER_VERIFIER)
	@$(DISC_READER_VERIFIER)

VERIFIER_SUPPORT := utils/assert.c

$(DISC_READER_VERIFIER): tests/verifyDiscReader.c engine/source/discReader.c \
		engine/source/virtualFileSystem.c engine/source/packageReader.c \
		engine/source/memoryArena.c engine/source/freestandingRuntime.c \
		utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyDiscReader.c engine/source/discReader.c \
		engine/source/virtualFileSystem.c engine/source/packageReader.c \
		engine/source/memoryArena.c engine/source/freestandingRuntime.c \
		utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(PACKAGE_READER_VERIFIER): tests/verifyPackageReader.c engine/source/packageReader.c \
		engine/source/memoryArena.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyPackageReader.c engine/source/packageReader.c \
		engine/source/memoryArena.c $(VERIFIER_SUPPORT) -o $@

$(RASTERIZER_VERIFIER): tests/verifyRasterizer.c render/software/rasterizer.c \
		render/software/rasterizerNEON.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyRasterizer.c render/software/rasterizer.c \
		render/software/rasterizerNEON.c $(VERIFIER_SUPPORT) -o $@

check:
	@scripts/checkNoDynamicAllocation.sh

clean:
	rm -rf $(BUILD_DIRECTORY)
