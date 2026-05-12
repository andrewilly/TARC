#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define DOCTEST_CONFIG_NO_POSIX_SIGNALS
#define DOCTEST_CONFIG_COLORS_NONE
#include "doctest/doctest.h"
#include "engine.h"
#include "io.h"
#include "types.h"

#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <random>
#include <algorithm>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Per-test sandbox: each test gets an isolated subdirectory, CWD is set there.
// ---------------------------------------------------------------------------

struct Sandbox {
    fs::path old_cwd;
    fs::path dir;

    Sandbox() {
        old_cwd = fs::current_path();
        auto tmp = fs::temp_directory_path();
        // unique-ish name per test case
        auto name = "tarc_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        dir = tmp / name;
        fs::create_directories(dir);
        fs::current_path(dir);
    }

    ~Sandbox() {
        fs::current_path(old_cwd);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string make_file(const std::string& name, size_t size, char fill = 'A') const {
        auto path = (dir / name).string();
        std::ofstream ofs(path, std::ios::binary);
        if (size > 0) {
            std::vector<char> buf(std::min(size, size_t(65536)), fill);
            size_t written = 0;
            while (written < size) {
                size_t chunk = std::min(buf.size(), size - written);
                ofs.write(buf.data(), chunk);
                written += chunk;
            }
        }
        return name; // return just the filename (relative to CWD)
    }

    std::string make_random_file(const std::string& name, size_t size) const {
        auto path = (dir / name).string();
        std::ofstream ofs(path, std::ios::binary);
        if (size > 0) {
            std::mt19937 rng(static_cast<unsigned int>(name.size() + size));
            std::vector<char> buf(std::min(size, size_t(65536)));
            for (auto& b : buf) b = static_cast<char>(rng() & 0xFF);
            size_t written = 0;
            while (written < size) {
                size_t chunk = std::min(buf.size(), size - written);
                ofs.write(buf.data(), chunk);
                written += chunk;
            }
        }
        return name;
    }

    std::string read_file(const fs::path& path) const {
        auto full = path.is_relative() ? (dir / path).string() : path.string();
        std::ifstream ifs(full, std::ios::binary | std::ios::ate);
        if (!ifs) return {};
        auto sz = ifs.tellg();
        std::string buf(sz, '\0');
        ifs.seekg(0);
        ifs.read(buf.data(), sz);
        return buf;
    }

    std::string arch_path(const std::string& name) const {
        return (dir / name).string();
    }

    bool exists(const fs::path& path) const {
        auto full = path.is_relative() ? (dir / path).string() : path.string();
        std::error_code ec;
        return fs::exists(full, ec);
    }

    uint64_t file_size(const fs::path& path) const {
        auto full = path.is_relative() ? (dir / path).string() : path.string();
        std::error_code ec;
        auto sz = fs::file_size(full, ec);
        return ec ? 0 : sz;
    }

    std::string out_dir(const std::string& name) const {
        auto d = dir / name;
        fs::create_directories(d);
        return d.string();
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("create and extract single small file")
{
    Sandbox sb;
    auto src = sb.make_file("small.txt", 1024);
    auto arc = sb.arch_path("small.strk");
    auto out = sb.out_dir("out_small");

    auto cr = Engine::compress(arc, {src});
    CHECK(cr.ok);
    CHECK(cr.error == TarcError::None);

    auto lr = Engine::list(arc);
    CHECK(lr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);

    auto dst = fs::path(out) / "small.txt";
    CHECK(sb.exists(dst.string()));
    CHECK(sb.read_file(dst.string()) == sb.read_file(src));
}

TEST_CASE("create and extract empty file")
{
    Sandbox sb;
    auto src = sb.make_file("empty.txt", 0);
    auto arc = sb.arch_path("empty.strk");
    auto out = sb.out_dir("out_empty");

    auto cr = Engine::compress(arc, {src});
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);

    auto dst = fs::path(out) / "empty.txt";
    CHECK(sb.exists(dst.string()));
    CHECK(sb.file_size(dst.string()) == 0);
}

TEST_CASE("create and extract multiple files")
{
    Sandbox sb;
    auto src1 = sb.make_file("multi_a.bin", 4096, 'X');
    auto src2 = sb.make_file("multi_b.bin", 8192, 'Y');
    auto arc  = sb.arch_path("multi.strk");
    auto out  = sb.out_dir("out_multi");

    auto cr = Engine::compress(arc, {src1, src2});
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);

    CHECK(sb.read_file(fs::path(out) / "multi_a.bin") == sb.read_file(src1));
    CHECK(sb.read_file(fs::path(out) / "multi_b.bin") == sb.read_file(src2));
}

TEST_CASE("list archive contents")
{
    Sandbox sb;
    auto src = sb.make_file("list_test.bin", 512, 'L');
    auto arc = sb.arch_path("list_test.strk");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    auto lr = Engine::list(arc);
    CHECK(lr.ok);
}

TEST_CASE("verify archive integrity")
{
    Sandbox sb;
    auto src = sb.make_file("verify_test.bin", 2048, 'V');
    auto arc = sb.arch_path("verify_test.strk");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {true, false, true, false, {}});
    CHECK(er.ok);
}

TEST_CASE("codec: STORE")
{
    Sandbox sb;
    auto src = sb.make_file("store_test.bin", 1024, 'S');
    auto arc = sb.arch_path("store_test.strk");
    auto out = sb.out_dir("out_store");

    CompressOptions opts;
    opts.codec = Codec::STORE;
    opts.has_codec_override = true;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "store_test.bin") == sb.read_file(src));
}

TEST_CASE("codec: LZMA")
{
    Sandbox sb;
    auto src = sb.make_random_file("lzma_test.bin", 8192);
    auto arc = sb.arch_path("lzma_test.strk");
    auto out = sb.out_dir("out_lzma");

    CompressOptions opts;
    opts.codec = Codec::LZMA;
    opts.has_codec_override = true;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "lzma_test.bin") == sb.read_file(src));
}

TEST_CASE("codec: ZSTD")
{
    Sandbox sb;
    auto src = sb.make_random_file("zstd_test.bin", 8192);
    auto arc = sb.arch_path("zstd_test.strk");
    auto out = sb.out_dir("out_zstd");

    CompressOptions opts;
    opts.codec = Codec::ZSTD;
    opts.has_codec_override = true;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "zstd_test.bin") == sb.read_file(src));
}

TEST_CASE("codec: LZ4")
{
    Sandbox sb;
    auto src = sb.make_random_file("lz4_test.bin", 16384);
    auto arc = sb.arch_path("lz4_test.strk");
    auto out = sb.out_dir("out_lz4");

    CompressOptions opts;
    opts.codec = Codec::LZ4;
    opts.has_codec_override = true;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "lz4_test.bin") == sb.read_file(src));
}

TEST_CASE("codec: Brotli")
{
    Sandbox sb;
    auto src = sb.make_random_file("br_test.bin", 8192);
    auto arc = sb.arch_path("br_test.strk");
    auto out = sb.out_dir("out_br");

    CompressOptions opts;
    opts.codec = Codec::BR;
    opts.has_codec_override = true;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "br_test.bin") == sb.read_file(src));
}

TEST_CASE("compression levels 1 and 19")
{
    Sandbox sb;
    auto src = sb.make_file("level_test.bin", 32768, 'R');
    auto arc = sb.arch_path("level_test.strk");
    auto out = sb.out_dir("out_level");

    CompressOptions opts;
    opts.level = 1;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);
    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "level_test.bin") == sb.read_file(src));

    // reset for level 19
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);

    opts.level = 19;
    cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);
    er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "level_test.bin") == sb.read_file(src));
}

TEST_CASE("extract with wildcard pattern")
{
    Sandbox sb;
    auto src1 = sb.make_file("alpha.txt", 256, 'A');
    auto src2 = sb.make_file("beta.txt", 256, 'B');
    auto src3 = sb.make_file("gamma.dat", 256, 'C');
    auto arc  = sb.arch_path("pattern.strk");
    auto out  = sb.out_dir("out_pattern");

    auto cr = Engine::compress(arc, {src1, src2, src3});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {"*.txt"}, {false, false, true, false, out});
    CHECK(er.ok);

    CHECK(sb.exists(fs::path(out) / "alpha.txt"));
    CHECK(sb.exists(fs::path(out) / "beta.txt"));
    CHECK(!sb.exists(fs::path(out) / "gamma.dat"));
}

TEST_CASE("error on non-existent file")
{
    Sandbox sb;
    auto arc = sb.arch_path("nonexistent.strk");
    auto cr = Engine::compress(arc, {"nonexistent_file_xyz"});
    CHECK(!cr.ok);
    CHECK(cr.error == TarcError::FileNotFound);
}

TEST_CASE("error on non-existent archive for extract")
{
    auto er = Engine::extract("/no/such/archive.strk");
    CHECK(!er.ok);
    CHECK(er.error == TarcError::FileNotFound);
}

TEST_CASE("error on corrupt archive")
{
    Sandbox sb;
    auto arc = sb.arch_path("corrupt.strk");
    {
        std::ofstream ofs(arc, std::ios::binary);
        ofs << "NOT A TARC ARCHIVE";
    }
    auto er = Engine::extract(arc);
    CHECK(!er.ok);
    CHECK(er.error == TarcError::InvalidHeader);
}

TEST_CASE("create with solid mode on/off")
{
    Sandbox sb;
    auto src = sb.make_random_file("solid_test.bin", 4096);
    auto arc = sb.arch_path("solid_test.strk");
    auto out = sb.out_dir("out_solid");

    CompressOptions opts;
    opts.solid_mode = true;
    auto cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);
    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "solid_test.bin") == sb.read_file(src));

    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);

    opts.solid_mode = false;
    cr = Engine::compress(arc, {src}, opts);
    CHECK(cr.ok);
    er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "solid_test.bin") == sb.read_file(src));
}

TEST_CASE("duplicate files are deduplicated")
{
    Sandbox sb;
    auto src = sb.make_file("dup_a.txt", 512, 'D');
    auto arc = sb.arch_path("dedup.strk");
    auto out = sb.out_dir("out_dedup");

    auto cr = Engine::compress(arc, {src, src});
    CHECK(cr.ok);
    Engine::CompressionStats s = Engine::get_stats();
    CHECK(s.duplicates_skipped >= 1);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "dup_a.txt") == sb.read_file(src));
}

TEST_CASE("read-only mode does not create output files")
{
    Sandbox sb;
    auto src = sb.make_file("test_only.txt", 128, 'T');
    auto arc = sb.arch_path("test_only.strk");
    auto out = sb.out_dir("out_test_only");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {true, false, true, false, out});
    CHECK(er.ok);

    // test-only mode should NOT create files even with output_dir
    CHECK(!sb.exists(fs::path(out) / "test_only.txt"));
}

// ============================================================================
// Additional Maturity Tests
// ============================================================================

TEST_CASE("empty file list returns error")
{
    Sandbox sb;
    auto arc = sb.arch_path("empty_list.strk");
    auto cr = Engine::compress(arc, {});
    CHECK(!cr.ok);
    CHECK(cr.error == TarcError::FileNotFound);
}

TEST_CASE("all files missing returns error")
{
    Sandbox sb;
    auto arc = sb.arch_path("no_files.strk");
    auto cr = Engine::compress(arc, {"nonexistent1.txt", "nonexistent2.txt"});
    CHECK(!cr.ok);
    CHECK(cr.error == TarcError::FileNotFound);
}

TEST_CASE("some files missing skips missing and compresses rest")
{
    Sandbox sb;
    auto src = sb.make_file("exists.txt", 64);
    auto arc = sb.arch_path("partial.strk");
    auto out = sb.out_dir("out_partial");

    auto cr = Engine::compress(arc, {src, "nonexistent.txt"});
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.exists(fs::path(out) / "exists.txt"));
}

TEST_CASE("nested directory structure round-trip")
{
    Sandbox sb;
    fs::create_directories(sb.dir / "sub" / "nested");
    auto src_path = "sub/nested/deep.txt";
    {
        std::ofstream ofs(sb.dir / src_path);
        ofs << "deep content";
    }
    auto arc = sb.arch_path("nested.strk");
    auto out = sb.out_dir("out_nested");

    auto cr = Engine::compress(arc, {src_path});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    REQUIRE(er.ok);

    CHECK(sb.exists(fs::path(out) / "sub" / "nested" / "deep.txt"));
    CHECK(sb.read_file(fs::path(out) / "sub" / "nested" / "deep.txt") == "deep content");
}

TEST_CASE("extract auto-creates output directory")
{
    Sandbox sb;
    auto src = sb.make_file("auto_dir.txt", 64);
    auto arc = sb.arch_path("auto_dir.strk");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    // output dir does not exist yet
    auto out = (sb.dir / "auto_created_out").string();
    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.exists(fs::path(out) / "auto_dir.txt"));
}

TEST_CASE("zero-length archive returns error")
{
    Sandbox sb;
    auto arc = sb.arch_path("empty_arc.strk");
    { std::ofstream ofs(arc); }
    auto er = Engine::extract(arc);
    CHECK(!er.ok);
}

TEST_CASE("truncated archive returns error")
{
    Sandbox sb;
    auto src = sb.make_file("trunc_test.bin", 1024);
    auto arc = sb.arch_path("trunc.strk");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    std::error_code ec;
    fs::resize_file(arc, sizeof(Header), ec);
    REQUIRE(!ec);

    auto er = Engine::extract(arc);
    CHECK(!er.ok);
}

TEST_CASE("corrupt chunk data returns error")
{
    Sandbox sb;
    auto src = sb.make_file("corrupt.bin", 4096, 'C');
    auto arc = sb.arch_path("corrupt.strk");

    CompressOptions opts;
    opts.codec = Codec::STORE;
    opts.has_codec_override = true;
    auto cr = Engine::compress(arc, {src}, opts);
    REQUIRE(cr.ok);

    auto fsize = fs::file_size(arc);
    REQUIRE(fsize > sizeof(Header) + sizeof(ChunkHeader));

    std::fstream f(arc, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(sizeof(Header) + sizeof(ChunkHeader) + 10, std::ios::beg);
    char orig;
    f.read(&orig, 1);
    f.seekp(-1, std::ios::cur);
    f.put(static_cast<char>(orig ^ 0xFF));
    f.close();

    auto er = Engine::extract(arc);
    CHECK(!er.ok);
    // Should fail with corruption error, not a file/header error
    CHECK(er.error != TarcError::FileNotFound);
    CHECK(er.error != TarcError::InvalidHeader);
}

TEST_CASE("unicode UTF-8 filename round-trip")
{
    Sandbox sb;
    std::string uname = "démo-文件-αβγ.txt";
    auto src = sb.make_file(uname, 256, 'U');
    auto arc = sb.arch_path("unicode.strk");
    auto out = sb.out_dir("out_unicode");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    REQUIRE(er.ok);

    CHECK(sb.exists(fs::path(out) / uname));
    CHECK(sb.read_file(fs::path(out) / uname) == sb.read_file(src));
}

TEST_CASE("filename with special characters round-trips")
{
    Sandbox sb;
    std::string special = "file with spaces_and_symbols!#$%&'().txt";
    auto src = sb.make_file(special, 128, 'S');
    auto arc = sb.arch_path("special.strk");
    auto out = sb.out_dir("out_special");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    REQUIRE(er.ok);

    CHECK(sb.exists(fs::path(out) / special));
}

TEST_CASE("security: sanitize_extract_path rejects traversal")
{
    CHECK(IO::sanitize_extract_path("../../etc/passwd").empty());
    CHECK(IO::sanitize_extract_path("subdir/../../../etc/passwd").empty());
    CHECK(!IO::sanitize_extract_path("normal/file.txt").empty());
    CHECK(!IO::sanitize_extract_path("file..txt").empty());
    CHECK(!IO::sanitize_extract_path(".../file.txt").empty());
}

TEST_CASE("security: is_safe_filename rejects dangerous names")
{
    CHECK(IO::is_safe_filename("normal.txt"));
    CHECK(!IO::is_safe_filename(std::string("bad\0name", 8)));
    CHECK(!IO::is_safe_filename("bad\x01name.txt"));
    CHECK(!IO::is_safe_filename(std::string(TARC_MAX_NAME_LEN + 1, 'a')));
}

TEST_CASE("single byte file with all codecs")
{
    Sandbox sb;
    auto src = sb.make_file("onebyte.txt", 1, 'Z');

    std::vector<Codec> codecs = {Codec::STORE, Codec::LZMA, Codec::ZSTD, Codec::LZ4, Codec::BR};
    for (auto codec : codecs) {
        auto arc = sb.arch_path("onebyte_" + std::to_string(static_cast<int>(codec)) + ".strk");
        auto out = sb.out_dir("out_onebyte_" + std::to_string(static_cast<int>(codec)));

        CompressOptions opts;
        opts.codec = codec;
        opts.has_codec_override = true;

        auto cr = Engine::compress(arc, {src}, opts);
        CHECK(cr.ok);

        auto er = Engine::extract(arc, {}, {false, false, true, false, out});
        CHECK(er.ok);
        CHECK(sb.read_file(fs::path(out) / "onebyte.txt") == sb.read_file(src));
    }
}

TEST_CASE("incompressible random data with all codecs")
{
    Sandbox sb;
    auto src = sb.make_random_file("random.dat", 16384);

    std::vector<Codec> codecs = {Codec::STORE, Codec::LZMA, Codec::ZSTD, Codec::LZ4, Codec::BR};
    for (auto codec : codecs) {
        auto arc = sb.arch_path("random_" + std::to_string(static_cast<int>(codec)) + ".strk");
        auto out = sb.out_dir("out_random_" + std::to_string(static_cast<int>(codec)));

        CompressOptions opts;
        opts.codec = codec;
        opts.has_codec_override = true;

        auto cr = Engine::compress(arc, {src}, opts);
        CHECK(cr.ok);

        auto er = Engine::extract(arc, {}, {false, false, true, false, out});
        CHECK(er.ok);
        CHECK(sb.read_file(fs::path(out) / "random.dat") == sb.read_file(src));
    }
}

TEST_CASE("three duplicate files all deduplicated")
{
    Sandbox sb;
    auto src = sb.make_file("dedup_multi.txt", 512, 'D');
    auto arc = sb.arch_path("dedup_multi.strk");
    auto out = sb.out_dir("out_dedup_multi");

    auto cr = Engine::compress(arc, {src, src, src});
    CHECK(cr.ok);
    auto s = Engine::get_stats();
    CHECK(s.duplicates_skipped >= 2);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.exists(fs::path(out) / "dedup_multi.txt"));
    CHECK(sb.read_file(fs::path(out) / "dedup_multi.txt") == sb.read_file(src));
}

TEST_CASE("many small files under STORE threshold")
{
    Sandbox sb;
    std::vector<std::string> files;
    for (int i = 0; i < 20; ++i) {
        files.push_back(sb.make_file("tiny_" + std::to_string(i) + ".txt", 100,
                                     static_cast<char>('A' + (i % 26))));
    }
    auto arc = sb.arch_path("many_tiny.strk");
    auto out = sb.out_dir("out_many_tiny");

    auto cr = Engine::compress(arc, files);
    CHECK(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);

    for (auto& f : files) {
        CHECK(sb.exists(fs::path(out) / f));
        CHECK(sb.read_file(fs::path(out) / f) == sb.read_file(f));
    }
}

TEST_CASE("list non-existent archive returns error")
{
    auto lr = Engine::list("/nonexistent/path/archive.strk");
    CHECK(!lr.ok);
    CHECK(lr.error == TarcError::FileNotFound);
}

TEST_CASE("higher compression level produces smaller output")
{
    Sandbox sb;
    auto src = sb.make_file("ratio_test.bin", 65536, 'R');

    CompressOptions opts1;
    opts1.level = 1;
    auto arc1 = sb.arch_path("level1.strk");
    auto cr1 = Engine::compress(arc1, {src}, opts1);
    REQUIRE(cr1.ok);

    CompressOptions opts19;
    opts19.level = 19;
    auto arc19 = sb.arch_path("level19.strk");
    auto cr19 = Engine::compress(arc19, {src}, opts19);
    REQUIRE(cr19.ok);

    auto size1 = fs::file_size(arc1);
    auto size19 = fs::file_size(arc19);
    CHECK(size19 <= size1);
}

TEST_CASE("no-verify mode extracts correctly")
{
    Sandbox sb;
    auto src = sb.make_file("noverify.bin", 2048, 'N');
    auto arc = sb.arch_path("noverify.strk");
    auto out = sb.out_dir("out_noverify");

    auto cr = Engine::compress(arc, {src});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, false, false, out});
    CHECK(er.ok);
    CHECK(sb.read_file(fs::path(out) / "noverify.bin") == sb.read_file(src));
}

TEST_CASE("extract with empty patterns extracts all")
{
    Sandbox sb;
    auto src1 = sb.make_file("alpha.txt", 64, 'A');
    auto src2 = sb.make_file("beta.txt", 64, 'B');
    auto arc = sb.arch_path("all_files.strk");
    auto out = sb.out_dir("out_all_files");

    auto cr = Engine::compress(arc, {src1, src2});
    REQUIRE(cr.ok);

    auto er = Engine::extract(arc, {}, {false, false, true, false, out});
    CHECK(er.ok);
    CHECK(sb.exists(fs::path(out) / "alpha.txt"));
    CHECK(sb.exists(fs::path(out) / "beta.txt"));
}
