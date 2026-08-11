
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
                  engine/source/resourceCollection.c engine/source/geometryReader.c \
                  engine/source/scenegraph.c engine/source/resourceNode.c \
                  engine/source/animationReader.c \
                  engine/source/textureReader.c engine/source/textureDecode.c \
                  engine/source/material.c \
                  engine/source/propertySet.c \
                  engine/source/resourceKeyList.c \
                  engine/source/wardrobe.c \
                  engine/source/debugMenu.c \
                  engine/source/resourceCache.c \
                  engine/source/fontReader.c \
                  engine/source/glyphRaster.c \
                  engine/source/builtinFont.c \
                  engine/source/fontAtlas.c \
                  engine/source/interfaceSurface.c \
                  engine/source/interfaceMenu.c \
                  engine/source/engineText.c \
                  utils/resourceHash.c engine/source/resourceIndex.c \
                  engine/source/compression.c \
                  engine/source/discReader.c \
                  engine/source/virtualFileSystem.c \
                  engine/source/discContent.c \
                  engine/source/installerReader.c \
                  engine/source/programReader.c \
                  engine/source/archiveReader.c \
                  engine/source/jpegReader.c \
                  engine/source/engineCore.c \
                  utils/strings.c utils/checksum.c

RENDER_BACKEND ?= openGLES2

ifeq ($(RENDER_BACKEND),software)
LINUX_RENDER_SOURCES := render/software/renderSoftware.c \
                        render/software/rasterizer.c \
                        render/software/rasterizerNEON.c \
                        platform/linux/linuxPresenterSoftware.c
LINUX_LIBRARIES := -lX11
else
LINUX_RENDER_SOURCES := render/openGLES2/renderOpenGLES2.c render/meshCamera.c \
                        platform/linux/linuxPresenterEGL.c
LINUX_LIBRARIES := -lEGL -lGLESv2 -lX11
endif

LINUX_SOURCES := $(ENGINE_SOURCES) \
                 $(LINUX_RENDER_SOURCES) \
                 platform/linux/linuxDiscStore.c platform/linux/linuxEntryPoint.c

WEB_SOURCES := $(ENGINE_SOURCES) \
               render/webGPU/renderWebGPU.c render/meshCamera.c \
               platform/web/webDiscStore.c platform/web/webEntryPoint.c

LINUX_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/linux
WEB_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/web
ARM_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/armv5

LINUX_EXECUTABLE := $(LINUX_OUTPUT_DIRECTORY)/victoriaSims
WEB_MODULE := $(WEB_OUTPUT_DIRECTORY)/victoriaSims.wasm
ARM_LIBRARY := $(ARM_OUTPUT_DIRECTORY)/libVictoriaEngine.a

WEB_LINEAR_MEMORY_BYTES ?= 136314880

WEB_EXPORTS := -Wl,--export=victoriaWebInitialize \
               -Wl,--export=victoriaWebResize \
               -Wl,--export=victoriaWebRenderFrame \
               -Wl,--export=victoriaWebShutdown \
               -Wl,--export=victoriaWebGetBudgetTotalBytes \
               -Wl,--export=victoriaWebGetBudgetUsedBytes \
               -Wl,--export=victoriaWebGetProfilerReportPointer \
               -Wl,--export=victoriaWebGetProfilerReportLength -Wl,--export=victoriaWebGetMenuTextPointer -Wl,--export=victoriaWebGetMenuTextLength -Wl,--export=victoriaWebHandleMenuKey -Wl,--export=victoriaWebHandlePointer \
               -Wl,--export=victoriaWebGetFrameMicroseconds \
               -Wl,--export=victoriaWebGetAverageFrameMicroseconds \
               -Wl,--export=victoriaWebGetWorstFrameMicroseconds \
               -Wl,--export=victoriaWebGetFrameIntervalMicroseconds \
               -Wl,--export=victoriaWebGetGraphicsMemoryLimitBytes \
               -Wl,--export=victoriaWebOpenDisc \
               -Wl,--export=victoriaWebStepDiscLoad \
               -Wl,--export=victoriaWebGetWantedOffset \
               -Wl,--export=victoriaWebGetWantedLength \
               -Wl,--export=victoriaWebGetDeliveryPointer \
               -Wl,--export=victoriaWebDeliver \
               -Wl,--export=victoriaWebGetGraphicsMemoryUsedBytes

WEB_FLAGS := --target=wasm32 -ffreestanding -nostdlib -fno-builtin \
             -DVICTORIA_FREESTANDING_BUILTINS=1
WEB_LINK_FLAGS := -Wl,--no-entry -Wl,--allow-undefined \
                  -Wl,--initial-memory=$(WEB_LINEAR_MEMORY_BYTES) \
                  -Wl,--max-memory=$(WEB_LINEAR_MEMORY_BYTES) \
                  $(WEB_EXPORTS)

ARM_LIBRARY_SOURCES := $(ENGINE_SOURCES) \
                       render/software/renderSoftware.c \
                       render/software/rasterizer.c \
                       render/software/rasterizerNEON.c

ARM_ARCHITECTURE ?= armv5te
ARM_FLAGS := -march=$(ARM_ARCHITECTURE) -mfloat-abi=soft -ffreestanding \
             -DVICTORIA_FREESTANDING_BUILTINS=1

OABI_FLAGS := -mabi=apcs-gnu $(ARM_FLAGS)
OABI_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/oabi
OABI_LIBRARY := $(OABI_OUTPUT_DIRECTORY)/libVictoriaEngine.a
OABI_COMPILER ?= $(ARM_COMPILER)
OABI_READELF ?= arm-linux-gnueabi-readelf

ARMV7_COMPILER ?= $(ARM_COMPILER)
ARMV7_ARCHITECTURE ?= armv7-a
ARMV7_TUNE ?= cortex-a8
ARMV7_FLAGS := -march=$(ARMV7_ARCHITECTURE) -mtune=$(ARMV7_TUNE) \
               -mfpu=neon -mfloat-abi=softfp -ffreestanding \
               -DVICTORIA_FREESTANDING_BUILTINS=1 -DVICTORIA_HAS_NEON=1
ARMV7_OUTPUT_DIRECTORY := $(BUILD_DIRECTORY)/armv7
ARMV7_LIBRARY := $(ARMV7_OUTPUT_DIRECTORY)/libVictoriaEngine.a
ARMV7_READELF ?= arm-linux-gnueabi-readelf

define buildEngineLibrary
@mkdir -p $(2)
@for source in $(ARM_LIBRARY_SOURCES); do \
	objectName=$(2)/`basename $$source .c`.o; \
	echo "$(1) -c $$source -o $$objectName"; \
	$(1) $(COMMON_FLAGS) $(3) -c $$source -o $$objectName || exit 1; \
done
$(ARCHIVER) rcs $(4) $(2)/*.o
endef

.PHONY: all linux web armv5 armv7 oabi verify verifyWeb check clean

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
	echo "$$attributes" | grep -qE 'Tag_CPU_arch: v7|Description: ARM v7' || \
		{ echo "ERROR: not ARMv7" >&2; exit 1; }; \
	echo "$$attributes" | grep -qE 'Tag_Advanced_SIMD_arch: NEON|Description: NEON' || \
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
FREESTANDING_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyFreestandingRuntime
PACKAGE_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyPackageReader
DISC_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyDiscReader
GEOMETRY_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyGeometryReader
MESH_CAMERA_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyMeshCamera
COMPRESSION_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyCompression
STRINGS_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyStrings
SCENEGRAPH_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyScenegraph
RESOURCE_NODE_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyResourceNode
ANIMATION_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyAnimationReader
TEXTURE_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyTexture
RESOURCE_INDEX_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyResourceIndex
RESOURCE_COLLECTION_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyResourceCollection
INSTALLER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyInstaller
PROGRAM_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyProgram
ARCHIVE_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyArchive
PROPERTY_SET_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyPropertySet
WARDROBE_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyWardrobe
DEBUG_MENU_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyDebugMenu
RESOURCE_CACHE_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyResourceCache
FONT_READER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyFontReader
GLYPH_RASTER_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyGlyphRaster
FONT_ATLAS_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyFontAtlas
INTERFACE_VERIFIER := $(BUILD_DIRECTORY)/tests/verifyInterface

verify: $(FREESTANDING_VERIFIER) $(RASTERIZER_VERIFIER) $(PACKAGE_READER_VERIFIER) $(DISC_READER_VERIFIER) \
		$(GEOMETRY_READER_VERIFIER) $(MESH_CAMERA_VERIFIER) \
		$(COMPRESSION_VERIFIER) $(STRINGS_VERIFIER) $(RESOURCE_COLLECTION_VERIFIER) \
		$(SCENEGRAPH_VERIFIER) $(RESOURCE_NODE_VERIFIER) $(ANIMATION_READER_VERIFIER) \
		$(TEXTURE_VERIFIER) \
		$(RESOURCE_INDEX_VERIFIER) $(INSTALLER_VERIFIER) $(PROGRAM_VERIFIER) $(ARCHIVE_VERIFIER) \
		$(PROPERTY_SET_VERIFIER) $(WARDROBE_VERIFIER) $(DEBUG_MENU_VERIFIER) \
		$(RESOURCE_CACHE_VERIFIER) $(FONT_READER_VERIFIER) $(GLYPH_RASTER_VERIFIER) \
		$(FONT_ATLAS_VERIFIER) $(INTERFACE_VERIFIER)
	@$(FREESTANDING_VERIFIER)
	@$(RASTERIZER_VERIFIER)
	@$(PACKAGE_READER_VERIFIER)
	@$(DISC_READER_VERIFIER)
	@$(GEOMETRY_READER_VERIFIER)
	@$(MESH_CAMERA_VERIFIER)
	@$(COMPRESSION_VERIFIER)
	@$(STRINGS_VERIFIER)
	@$(SCENEGRAPH_VERIFIER)
	@$(RESOURCE_NODE_VERIFIER)
	@$(ANIMATION_READER_VERIFIER)
	@$(TEXTURE_VERIFIER)
	@$(RESOURCE_INDEX_VERIFIER)
	@$(RESOURCE_COLLECTION_VERIFIER)
	@$(INSTALLER_VERIFIER)
	@$(PROGRAM_VERIFIER)
	@$(ARCHIVE_VERIFIER)
	@$(PROPERTY_SET_VERIFIER)
	@$(WARDROBE_VERIFIER)
	@$(DEBUG_MENU_VERIFIER)
	@$(RESOURCE_CACHE_VERIFIER)
	@$(FONT_READER_VERIFIER)
	@$(GLYPH_RASTER_VERIFIER)
	@$(FONT_ATLAS_VERIFIER)
	@$(INTERFACE_VERIFIER)

verifyWeb: $(WEB_MODULE)
	@node tests/verifyRuntimeUpload.mjs
	@node tests/verifyWebModule.mjs

VERIFIER_SUPPORT := utils/assert.c

$(ARCHIVE_VERIFIER): tests/verifyArchive.c engine/source/archiveReader.c utils/strings.c \
		$(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyArchive.c engine/source/archiveReader.c \
		utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(PROGRAM_VERIFIER): tests/verifyProgram.c engine/source/programReader.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyProgram.c engine/source/programReader.c \
		$(VERIFIER_SUPPORT) -o $@

$(INSTALLER_VERIFIER): tests/verifyInstaller.c engine/source/installerReader.c \
		utils/checksum.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyInstaller.c engine/source/installerReader.c \
		utils/checksum.c utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(RESOURCE_COLLECTION_VERIFIER): tests/verifyResourceCollection.c engine/source/resourceCollection.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyResourceCollection.c \
		engine/source/resourceCollection.c engine/source/packageReader.c \
		engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(RESOURCE_INDEX_VERIFIER): tests/verifyResourceIndex.c engine/source/resourceIndex.c \
		engine/source/virtualFileSystem.c engine/source/memoryArena.c \
		utils/strings.c utils/resourceHash.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyResourceIndex.c engine/source/resourceIndex.c \
		engine/source/virtualFileSystem.c engine/source/memoryArena.c \
		utils/strings.c utils/resourceHash.c $(VERIFIER_SUPPORT) -o $@

$(TEXTURE_VERIFIER): tests/verifyTexture.c engine/source/textureReader.c \
		engine/source/textureDecode.c engine/source/material.c engine/source/resourceCollection.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyTexture.c engine/source/textureReader.c \
		engine/source/textureDecode.c engine/source/material.c engine/source/resourceCollection.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c \
		$(VERIFIER_SUPPORT) -o $@

$(ANIMATION_READER_VERIFIER): tests/verifyAnimationReader.c engine/source/animationReader.c \
		engine/source/resourceCollection.c engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyAnimationReader.c engine/source/animationReader.c \
		engine/source/resourceCollection.c engine/source/memoryArena.c utils/strings.c \
		$(VERIFIER_SUPPORT) -o $@

$(RESOURCE_NODE_VERIFIER): tests/verifyResourceNode.c engine/source/resourceNode.c \
		engine/source/resourceCollection.c engine/source/compression.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyResourceNode.c engine/source/resourceNode.c \
		engine/source/resourceCollection.c engine/source/compression.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c \
		$(VERIFIER_SUPPORT) -o $@

$(SCENEGRAPH_VERIFIER): tests/verifyScenegraph.c engine/source/scenegraph.c \
		engine/source/resourceCollection.c engine/source/geometryReader.c \
		engine/source/compression.c engine/source/packageReader.c \
		engine/source/memoryArena.c utils/strings.c utils/resourceHash.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyScenegraph.c engine/source/scenegraph.c \
		engine/source/resourceCollection.c engine/source/geometryReader.c \
		engine/source/compression.c engine/source/packageReader.c \
		engine/source/memoryArena.c utils/strings.c utils/resourceHash.c $(VERIFIER_SUPPORT) -o $@

$(STRINGS_VERIFIER): tests/verifyStrings.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyStrings.c utils/strings.c \
		$(VERIFIER_SUPPORT) -o $@

$(COMPRESSION_VERIFIER): tests/verifyCompression.c engine/source/compression.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyCompression.c engine/source/compression.c \
		$(VERIFIER_SUPPORT) -o $@

$(MESH_CAMERA_VERIFIER): tests/verifyMeshCamera.c render/meshCamera.c \
		engine/source/freestandingRuntime.c engine/source/resourceCollection.c engine/source/geometryReader.c \
		engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyMeshCamera.c render/meshCamera.c \
		engine/source/freestandingRuntime.c engine/source/resourceCollection.c engine/source/geometryReader.c \
		engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(GEOMETRY_READER_VERIFIER): tests/verifyGeometryReader.c engine/source/resourceCollection.c engine/source/geometryReader.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyGeometryReader.c engine/source/resourceCollection.c engine/source/geometryReader.c \
		engine/source/packageReader.c engine/source/memoryArena.c utils/strings.c \
		$(VERIFIER_SUPPORT) -o $@

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

$(PROPERTY_SET_VERIFIER): tests/verifyPropertySet.c engine/source/propertySet.c utils/strings.c \
		$(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyPropertySet.c engine/source/propertySet.c \
		utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(WARDROBE_VERIFIER): tests/verifyWardrobe.c engine/source/wardrobe.c utils/strings.c \
		$(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyWardrobe.c engine/source/wardrobe.c \
		utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(DEBUG_MENU_VERIFIER): tests/verifyDebugMenu.c engine/source/debugMenu.c utils/strings.c \
		$(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyDebugMenu.c engine/source/debugMenu.c \
		utils/strings.c $(VERIFIER_SUPPORT) -o $@

$(RESOURCE_CACHE_VERIFIER): tests/verifyResourceCache.c engine/source/resourceCache.c \
		engine/source/memoryArena.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyResourceCache.c engine/source/resourceCache.c \
		engine/source/memoryArena.c $(VERIFIER_SUPPORT) -o $@

FONT_SOURCES := engine/source/fontReader.c engine/source/glyphRaster.c \
                engine/source/builtinFont.c engine/source/fontAtlas.c \
                utils/checksum.c

INTERFACE_SOURCES := $(FONT_SOURCES) engine/source/interfaceSurface.c \
                     engine/source/interfaceMenu.c engine/source/debugMenu.c \
                     utils/strings.c

$(FONT_READER_VERIFIER): tests/verifyFontReader.c engine/source/fontReader.c $(VERIFIER_SUPPORT) \
		testAssets/fonts/fixture.mxf
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyFontReader.c engine/source/fontReader.c \
		$(VERIFIER_SUPPORT) -o $@

$(GLYPH_RASTER_VERIFIER): tests/verifyGlyphRaster.c engine/source/glyphRaster.c \
		engine/source/fontReader.c $(VERIFIER_SUPPORT) testAssets/fonts/fixture.mxf
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyGlyphRaster.c engine/source/glyphRaster.c \
		engine/source/fontReader.c $(VERIFIER_SUPPORT) -o $@

$(FONT_ATLAS_VERIFIER): tests/verifyFontAtlas.c $(FONT_SOURCES) utils/strings.c \
		$(VERIFIER_SUPPORT) testAssets/fonts/fixture.mxf
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyFontAtlas.c $(FONT_SOURCES) utils/strings.c \
		$(VERIFIER_SUPPORT) -o $@

$(INTERFACE_VERIFIER): tests/verifyInterface.c $(INTERFACE_SOURCES) $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyInterface.c $(INTERFACE_SOURCES) \
		$(VERIFIER_SUPPORT) -o $@

testAssets/fonts/fixture.mxf: scripts/makeFontFixture.ts scripts/src/truetype/*.ts
	node scripts/makeFontFixture.ts

$(FREESTANDING_VERIFIER): tests/verifyFreestandingRuntime.c engine/source/freestandingRuntime.c \
		$(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyFreestandingRuntime.c \
		engine/source/freestandingRuntime.c $(VERIFIER_SUPPORT) -o $@

$(RASTERIZER_VERIFIER): tests/verifyRasterizer.c render/software/rasterizer.c \
		render/software/rasterizerNEON.c $(VERIFIER_SUPPORT)
	@mkdir -p $(BUILD_DIRECTORY)/tests
	$(HOST_COMPILER) $(COMMON_FLAGS) tests/verifyRasterizer.c render/software/rasterizer.c \
		render/software/rasterizerNEON.c $(VERIFIER_SUPPORT) -o $@

check:
	@node scripts/checkNoDynamicAllocation.ts
	@node scripts/checkNoFloatingPoint.ts

clean:
	rm -rf $(BUILD_DIRECTORY)
