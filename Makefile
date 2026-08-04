# Victoria Sims build.
#
#   make linux    native OpenGL ES 2.0 executable (default)
#   make web      freestanding wasm32 module plus its host page
#   make armv5    ARMv5TE cross build of the portable engine core
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
INCLUDE_FLAGS := -Iengine/include

WARNING_FLAGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
                 -Wpointer-arith -Wcast-qual -Wwrite-strings
STANDARD_FLAGS := -std=c99 -pedantic
OPTIMIZATION_FLAGS ?= -O2
EXTRA_FLAGS ?=

COMMON_FLAGS := $(STANDARD_FLAGS) $(WARNING_FLAGS) $(OPTIMIZATION_FLAGS) $(INCLUDE_FLAGS) $(EXTRA_FLAGS)

ENGINE_SOURCES := engine/source/memoryArena.c \
                  engine/source/memoryBudget.c \
                  engine/source/freestandingRuntime.c \
                  engine/source/engineCore.c

LINUX_SOURCES := $(ENGINE_SOURCES) \
                 render/openGLES2/renderOpenGLES2.c \
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

LINUX_LIBRARIES := -lEGL -lGLESv2 -lX11

# The wasm module owns exactly the reserved budget plus a fixed slack for the
# shadow stack and static data. Both bounds are pinned so the module can never
# grow past the ceiling at run time.
WEB_LINEAR_MEMORY_BYTES ?= 135266304

WEB_EXPORTS := -Wl,--export=victoriaWebInitialize \
               -Wl,--export=victoriaWebResize \
               -Wl,--export=victoriaWebRenderFrame \
               -Wl,--export=victoriaWebShutdown \
               -Wl,--export=victoriaWebGetBudgetTotalBytes \
               -Wl,--export=victoriaWebGetBudgetUsedBytes

WEB_FLAGS := --target=wasm32 -ffreestanding -nostdlib -fno-builtin \
             -DVICTORIA_FREESTANDING_BUILTINS=1
WEB_LINK_FLAGS := -Wl,--no-entry -Wl,--allow-undefined \
                  -Wl,--initial-memory=$(WEB_LINEAR_MEMORY_BYTES) \
                  -Wl,--max-memory=$(WEB_LINEAR_MEMORY_BYTES) \
                  $(WEB_EXPORTS)

# ARMv5TE, software floating point: the oldest hardware in scope has no
# floating point unit worth targeting.
ARM_FLAGS := -march=armv5te -mfloat-abi=soft -ffreestanding \
             -DVICTORIA_FREESTANDING_BUILTINS=1

.PHONY: all linux web armv5 check clean

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

$(ARM_LIBRARY): $(ENGINE_SOURCES)
	@mkdir -p $(ARM_OUTPUT_DIRECTORY)
	@for source in $(ENGINE_SOURCES); do \
		objectName=$(ARM_OUTPUT_DIRECTORY)/`basename $$source .c`.o; \
		echo "$(ARM_COMPILER) -c $$source -o $$objectName"; \
		$(ARM_COMPILER) $(COMMON_FLAGS) $(ARM_FLAGS) -c $$source -o $$objectName || exit 1; \
	done
	$(ARCHIVER) rcs $@ $(ARM_OUTPUT_DIRECTORY)/*.o

check:
	@tools/checkNoDynamicAllocation.sh

clean:
	rm -rf $(BUILD_DIRECTORY)
