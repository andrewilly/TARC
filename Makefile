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

# Release / Debug / Sanitize
ifdef ASAN
    CXXFLAGS += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DASAN
    CFLAGS   += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
    LDFLAGS  += -fsanitize=address,undefined
else ifdef DEBUG
    CXXFLAGS += -g -O0 -DDEBUG
    CFLAGS   += -g -O0
else
    CXXFLAGS += -O3 -DNDEBUG -flto
    CFLAGS   += -O3 -DNDEBUG -flto
    LDFLAGS  += -flto
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
SOURCES   = $(SRCDIR)/main.cpp $(SRCDIR)/ui.cpp \
           $(SRCDIR)/io.cpp $(SRCDIR)/engine.cpp

TEST_SRC = test/tests.cpp $(SRCDIR)/ui.cpp \
           $(SRCDIR)/io.cpp $(SRCDIR)/engine.cpp

SFX_SRC  = $(SRCDIR)/sfx_stub.cpp $(SRCDIR)/ui.cpp \
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
.PHONY: all clean sfx test install info asan ubsan

all: info $(TARGET)

info:
	@echo ""
	@echo "  TARC STRIKE v2.11 — Build"
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

asan ubsan:
	$(MAKE) ASAN=1 test

FUZZ_TGT = fuzz_archive$(EXE_EXT)
FUZZ_SRC = fuzz/fuzz_archive.cpp
FUZZ_OBJ = $(OBJDIR)/fuzz_archive.o \
           $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/fuzz_%.o,$(filter-out $(SRCDIR)/main.cpp,$(SOURCES))) \
           $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/fuzz_%.o,$(C_SOURCES))

# Check if libFuzzer is available (Apple Clang often lacks it)
HAVE_LIBFUZZER := $(shell echo 'int main(){}' | $(CXX) -x c++ -fsanitize=fuzzer -o /dev/null - 2>/dev/null && echo yes || echo no)

.PHONY: fuzz
fuzz: CXXFLAGS += -g -O1 -fno-omit-frame-pointer
fuzz: CFLAGS   += -g -O1 -fno-omit-frame-pointer
ifeq ($(HAVE_LIBFUZZER),yes)
fuzz: CXXFLAGS += -fsanitize=fuzzer,address,undefined -D__LIBFUZZER__=1
fuzz: CFLAGS   += -fsanitize=fuzzer,address,undefined
fuzz: LDFLAGS  += -fsanitize=fuzzer,address,undefined
else
fuzz: CXXFLAGS += -fsanitize=address,undefined
fuzz: CFLAGS   += -fsanitize=address,undefined
fuzz: LDFLAGS  += -fsanitize=address,undefined
endif
fuzz: $(FUZZ_TGT)
	@echo ""
	@echo "  Fuzz target ready: $(FUZZ_TGT)"
ifeq ($(HAVE_LIBFUZZER),yes)
	@echo "  Run: ./$(FUZZ_TGT) [corpus-dir]"
else
	@echo "  Run: ./$(FUZZ_TGT) <seed-file> [seed-file...]"
	@echo "  (libFuzzer not available; using standalone driver)"
	@echo "  Install LLVM Clang for libFuzzer: brew install llvm"
endif
	@echo ""

$(OBJDIR)/fuzz_%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/fuzz_%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/fuzz_archive.o: fuzz/fuzz_archive.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(FUZZ_TGT): $(FUZZ_OBJ)
	$(CXX) $(FUZZ_OBJ) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TEST_TGT) $(SFX_TGT)
	@echo "  Clean done."

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "  Installed: /usr/local/bin/$(TARGET)"
