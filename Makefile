PLATFORM ?= PLATFORM_DESKTOP
BUILD_MODE ?= RELEASE
WEB_OUT_DIR ?= build/web
DESKTOP_OUT_DIR ?= .
RAYLIB_DIR = C:/raylib
EMSDK_DIR ?= C:/raylib/emsdk
INCLUDE_DIR = -I ./ -I $(RAYLIB_DIR)/raylib/src -I $(RAYLIB_DIR)/raygui/src
LIBRARY_DIR = -L $(RAYLIB_DIR)/raylib/src
DEFINES = -D _DEFAULT_SOURCE -D RAYLIB_BUILD_MODE=$(BUILD_MODE) -D $(PLATFORM)

TARGET_BASENAME = controller

ifeq ($(PLATFORM),PLATFORM_DESKTOP)
    CC = g++
    EXT = .exe
    OUT_DIR = $(DESKTOP_OUT_DIR)
    ifeq ($(BUILD_MODE),RELEASE)
        CFLAGS ?= $(DEFINES) -ffast-math -march=native -D NDEBUG -O3 $(RAYLIB_DIR)/raylib/src/raylib.rc.data $(INCLUDE_DIR) $(LIBRARY_DIR) 
	else
        CFLAGS ?= $(DEFINES) -g $(RAYLIB_DIR)/raylib/src/raylib.rc.data $(INCLUDE_DIR) $(LIBRARY_DIR) 
	endif
    LIBS = -lraylib -lopengl32 -lgdi32 -lwinmm
endif

ifeq ($(PLATFORM),PLATFORM_WEB)
    CC = $(EMSDK_DIR)/upstream/emscripten/emcc.bat
    EXT = .html
    OUT_DIR = $(WEB_OUT_DIR)
    CFLAGS ?= $(DEFINES) $(RAYLIB_DIR)/raylib/src/libraylib.web.a -ffast-math -D NDEBUG -O3 -s USE_GLFW=3 -s FORCE_FILESYSTEM=1 -s MAX_WEBGL_VERSION=2 -s ALLOW_MEMORY_GROWTH=1 --preload-file $(dir $<)resources@resources --shell-file ./shell.html $(INCLUDE_DIR) $(LIBRARY_DIR)
endif

SOURCE = $(wildcard *.cpp)
HEADER = $(wildcard *.h)
OUTPUT = $(OUT_DIR)/$(TARGET_BASENAME)$(EXT)

.PHONY: all

.RECIPEPREFIX := >

all: controller

controller: $(SOURCE) $(HEADER)
>if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
>$(CC) -o $(OUTPUT) $(SOURCE) $(CFLAGS) $(LIBS) 

clean:
>if exist "$(TARGET_BASENAME).exe" del /F /Q "$(TARGET_BASENAME).exe"
>if exist "$(TARGET_BASENAME).html" del /F /Q "$(TARGET_BASENAME).html"
>if exist "$(WEB_OUT_DIR)/$(TARGET_BASENAME).html" del /F /Q "$(WEB_OUT_DIR)/$(TARGET_BASENAME).html"
>if exist "$(WEB_OUT_DIR)/$(TARGET_BASENAME).js" del /F /Q "$(WEB_OUT_DIR)/$(TARGET_BASENAME).js"
>if exist "$(WEB_OUT_DIR)/$(TARGET_BASENAME).wasm" del /F /Q "$(WEB_OUT_DIR)/$(TARGET_BASENAME).wasm"
>if exist "$(WEB_OUT_DIR)/$(TARGET_BASENAME).data" del /F /Q "$(WEB_OUT_DIR)/$(TARGET_BASENAME).data"