#include "engine.h"
#include "io.h"
#include "types.h"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

static bool g_initialized = false;

static int fuzz_archive(const uint8_t* data, size_t size) {
    if (size < 4) return 0;

    char tmpl[] = "/tmp/tarc_fuzz_XXXXXX.strk";
    int fd = mkstemps(tmpl, 5);
    if (fd < 0) return 0;

    std::string path(tmpl);
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data), size);
    }
    close(fd);

    if (!g_initialized) {
        g_initialized = true;
        Engine::reset_stats();
    }

    Engine::extract(path, {}, {true, false, false, false, ""});

    Engine::list(path, 0);

    std::remove(path.c_str());

    return 0;
}

#ifdef __LIBFUZZER__
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_archive(data, size);
}
#else
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <seed-file>\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "Cannot open: %s\n", argv[i]); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            std::vector<uint8_t> buf(sz);
            fread(buf.data(), 1, sz, f);
            fuzz_archive(buf.data(), buf.size());
        }
        fclose(f);
    }
    return 0;
}
#endif
