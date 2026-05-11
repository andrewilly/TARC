# ============================================================================
# TARC STRIKE v2.10 — Cross-Platform Makefile
# ============================================================================
# Supporta: macOS (Intel + Apple Silicon), Linux, Windows (MinGW/MSYS2)
#
# Uso:
#   make              Compila tarc (release)
#   make debug        Compila con simboli debug (-g -O0)
#   make clean        Pulisci i file oggetto e il binario
#   make sfx          Compila anche il stub SFX
#   make install      Installa in /usr/local/bin (sudo make install)
#
# Su macOS con Intel:    make       → produce binario x86_64
# Su macOS con M1/M2:    make       → produce binario arm64
# Su Linux:              make       → produce binario x86_64 con -march=native
# Su Windows (MinGW):    make       → produce tarc.exe
# ============================================================================

# Detect OS
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# Detect architecture
UNAME_M := $(shell uname -m 2>/dev/null || echo x86_64)

# ============================================================================
# Compiler & Flags
# ============================================================================
CXX      = c++
CC       = cc
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-unused-variable
CFLAGS   = -std=c11 -Wall

# Release / Debug
ifdef DEBUG
    CXXFLAGS += -g -O0 -DDEBUG
    CFLAGS   += -g -O0
else
    CXXFLAGS += -O3 -DNDEBUG
    CFLAGS   += -O3 -DNDEBUG
endif

# ============================================================================
# Platform-specific flags
# ============================================================================
ifeq ($(UNAME_S),Darwin)
    # macOS — detecta automaticamente Intel (x86_64) o Apple Silicon (arm64)
    CXXFLAGS += -D_DARWIN_C_SOURCE -march=native -ftree-vectorize -funroll-loops
    CXXFLAGS += -fno-math-errno -fno-trapping-math
    LDFLAGS  += -ldl -lzstd -llz4 -llzma -lbrotlienc -lbrotlidec -lbrotlicommon
    # Su macOS NON usare -s (strip) perche' rimuove i simboli necessari per dSYM
    EXE_EXT  =
    MSG_OS   = macOS ($(UNAME_M))
else ifeq ($(UNAME_S),Linux)
    # Linux
    CXXFLAGS += -D_GNU_SOURCE -march=native -mtune=generic -ftree-vectorize -funroll-loops
    CXXFLAGS += -fno-math-errno -fno-trapping-math
    LDFLAGS  += -ldl -lpthread -lzstd -llz4 -llzma -lbrotlienc -lbrotlidec -lbrotlicommon
    ifeq ($(DEBUG),)
        LDFLAGS += -s
    endif
    EXE_EXT  =
    MSG_OS   = Linux ($(UNAME_M))
else ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)))
    # Windows — MinGW / MSYS2
    CXXFLAGS += -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_USE_MATH_DEFINES
    CXXFLAGS += -march=native -ftree-vectorize
    LDFLAGS  += -lbcrypt -luser32 -lzstd -llz4 -llzma -lbrotlienc -lbrotlidec -lbrotlicommon -static
    EXE_EXT  = .exe
    MSG_OS   = Windows (MinGW)
else
    # Fallback generico
    CXXFLAGS += -march=native
    LDFLAGS  += -lpthread -lzstd -llz4 -llzma -lbrotlienc -lbrotlidec -lbrotlicommon
    EXE_EXT  =
    MSG_OS   = $(UNAME_S)
endif

# ============================================================================
# Include paths
# ============================================================================
# xxhash e' incluso nel progetto (include/xxhash.h + src/xxhash.c).
# Le altre librerie (zstd, lz4, lzma, brotli) devono essere installate:
#
# Su macOS con Homebrew:
#   brew install zstd lz4 xz brotli
#
# Su Ubuntu/Debian:
#   sudo apt install libzstd-dev liblz4-dev liblzma-dev libbrotli-dev
#
# Su Fedora:
#   sudo dnf install libzstd-devel lz4-devel xz-devel brotli-devel
# ============================================================================

# Homebrew paths (macOS)
ifneq ($(wildcard /opt/homebrew/include),)
    # Apple Silicon Homebrew
    INCLUDES += -I/opt/homebrew/include
    LDFLAGS  += -L/opt/homebrew/lib
endif
ifneq ($(wildcard /usr/local/include),)
    # Intel Homebrew
    INCLUDES += -I/usr/local/include
    LDFLAGS  += -L/usr/local/lib
endif

INCLUDES += -Iinclude

# ============================================================================
# Source files
# ============================================================================
SRCDIR   = src
OBJDIR   = build

C_SOURCES = $(SRCDIR)/xxhash.c
SOURCES   = $(SRCDIR)/main.cpp $(SRCDIR)/ui.cpp $(SRCDIR)/license.cpp \
           $(SRCDIR)/io.cpp $(SRCDIR)/engine.cpp

TEST_SRC = test/tests.cpp $(SRCDIR)/ui.cpp $(SRCDIR)/license.cpp \
           $(SRCDIR)/io.cpp $(SRCDIR)/engine.cpp

SFX_SRC  = $(SRCDIR)/sfx_stub.cpp $(SRCDIR)/ui.cpp $(SRCDIR)/license.cpp \
           $(SRCDIR)/io.cpp $(SRCDIR)/engine.cpp

OBJECTS  = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SOURCES)) \
           $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(C_SOURCES))
TEST_OBJ = $(OBJDIR)/test_tests.o \
           $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(filter $(SRCDIR)/%,$(TEST_SRC))) \
           $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(C_SOURCES))
SFX_OBJ  = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/sfx_%.o,$(SFX_SRC)) \
           $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/sfx_%.o,$(C_SOURCES))

TARGET   = tarc$(EXE_EXT)
TEST_TGT = tarc_test$(EXE_EXT)
SFX_TGT  = tarc_sfx_stub$(EXE_EXT)

# ============================================================================
# Build rules
# ============================================================================
.PHONY: all clean sfx test install info

all: info $(TARGET)

info:
	@echo ""
	@echo "  TARC STRIKE v2.10 — Build"
	@echo "  OS:        $(MSG_OS)"
	@echo "  Compiler:  $(CXX)"
	@echo "  Flags:     $(CXXFLAGS)"
	@echo "  Target:    $(TARGET)"
	@echo ""

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "  Build OK: $(TARGET)"
	@echo ""
	@echo ""

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

sfx: $(SFX_OBJ)
	$(CXX) $(SFX_OBJ) -o $(SFX_TGT) $(LDFLAGS)
	@echo "  Build OK: $(SFX_TGT)"

test: $(TEST_TGT)
	@echo ""
	./$(TEST_TGT)
	@echo ""

$(TEST_TGT): $(TEST_OBJ)
	$(CXX) $(TEST_OBJ) -o $@ $(LDFLAGS)
	@echo "  Build OK: $(TEST_TGT)"

$(OBJDIR)/test_%.o: test/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/sfx_%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DTARC_SFX_STUB=1 -c $< -o $@

$(OBJDIR)/sfx_%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TEST_TGT) $(SFX_TGT)
	@echo "  Clean done."

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "  Installed: /usr/local/bin/$(TARGET)"
