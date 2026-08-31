#include <mcdk/config.hpp>
#include <mcdk/project_archive.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unzip.h>

#ifdef _WIN32
#include <iowin32.h>
#endif

namespace {

    namespace fs = std::filesystem;
    using mcdk::project::ConflictPolicy;
    using mcdk::project::ExportMode;
    using mcdk::project::ExportRequest;
    using mcdk::project::ProjectError;
    using mcdk::project::ProjectErrorCode;

    struct ZipEntry {
        std::string name;
        uLong       flag{};
    };

    class TemporaryDirectory final {
    public:
        explicit TemporaryDirectory(std::string_view label) {
            static std::atomic<std::uint64_t> serial{0};
            path_ = fs::temp_directory_path()
                  / ("mcdk-project-archive-" + std::string(label) + '-'
                     + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-'
                     + std::to_string(serial.fetch_add(1)));
            fs::create_directories(path_);
        }

        TemporaryDirectory(const TemporaryDirectory&)            = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        ~TemporaryDirectory() {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }

        [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    private:
        fs::path path_;
    };

    void require(bool condition, std::string message) {
        if (!condition) {
            throw std::runtime_error(std::move(message));
        }
    }

    [[nodiscard]] fs::path utf8Path(std::string_view value) {
        return fs::path(std::u8string(
            reinterpret_cast<const char8_t*>(value.data()),
            reinterpret_cast<const char8_t*>(value.data() + value.size())
        ));
    }

    void writeText(const fs::path& path, std::string_view value) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "unable to create test file: " + path.string());
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        require(static_cast<bool>(output), "unable to write test file: " + path.string());
    }

    [[nodiscard]] std::string manifest(
        std::string_view name,
        std::string_view headerUuid,
        std::string_view moduleUuid,
        std::string_view moduleType = "data"
    ) {
        return R"({
  "format_version": 2,
  "header": {
    "name": ")"
             + std::string(name) + R"(",
    "description": "archive test",
    "uuid": ")"
             + std::string(headerUuid) + R"(",
    "version": [1, 0, 0],
    "min_engine_version": [1, 20, 0]
  },
  "modules": [
    {
      "type": ")"
             + std::string(moduleType) + R"(",
      "uuid": ")"
             + std::string(moduleUuid) + R"(",
      "version": [1, 0, 0]
    }
  ]
})";
    }

    [[nodiscard]] unzFile openZip(const fs::path& path) {
#ifdef _WIN32
        zlib_filefunc64_def fileFunctions{};
        fill_win32_filefunc64W(&fileFunctions);
        return unzOpen2_64(path.c_str(), &fileFunctions);
#else
        return unzOpen64(path.c_str());
#endif
    }

    [[nodiscard]] std::vector<ZipEntry> zipEntries(const fs::path& path) {
        unzFile archive = openZip(path);
        require(archive != nullptr, "unable to open generated ZIP");
        std::vector<ZipEntry> result;
        for (int status = unzGoToFirstFile(archive); status == UNZ_OK; status = unzGoToNextFile(archive)) {
            unz_file_info64 information{};
            require(
                unzGetCurrentFileInfo64(archive, &information, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK,
                "unable to inspect generated ZIP entry"
            );
            std::vector<char> name(information.size_filename + 1, '\0');
            require(
                unzGetCurrentFileInfo64(
                    archive,
                    &information,
                    name.data(),
                    static_cast<uLong>(name.size()),
                    nullptr,
                    0,
                    nullptr,
                    0
                ) == UNZ_OK,
                "unable to read generated ZIP entry name"
            );
            result.push_back({std::string(name.data(), information.size_filename), information.flag});
        }
        require(unzClose(archive) == UNZ_OK, "unable to close generated ZIP");
        return result;
    }

    [[nodiscard]] std::string zipEntryContent(const fs::path& path, std::string_view wanted) {
        unzFile archive = openZip(path);
        require(archive != nullptr, "unable to open generated ZIP");
        require(
            unzLocateFile(archive, std::string(wanted).c_str(), 1) == UNZ_OK,
            "generated ZIP entry is missing: " + std::string(wanted)
        );
        require(unzOpenCurrentFile(archive) == UNZ_OK, "unable to open generated ZIP entry");
        std::string            result;
        std::array<char, 4096> buffer{};
        for (;;) {
            const int count = unzReadCurrentFile(archive, buffer.data(), static_cast<unsigned int>(buffer.size()));
            require(count >= 0, "unable to read generated ZIP entry");
            if (count == 0) {
                break;
            }
            result.append(buffer.data(), static_cast<std::size_t>(count));
        }
        require(unzCloseCurrentFile(archive) == UNZ_OK, "unable to close generated ZIP entry");
        require(unzClose(archive) == UNZ_OK, "unable to close generated ZIP");
        return result;
    }

    [[nodiscard]] bool contains(const std::vector<ZipEntry>& entries, std::string_view name) {
        return std::ranges::any_of(entries, [&](const ZipEntry& entry) { return entry.name == name; });
    }

    void testGlobAndFileNameRules() {
        using namespace mcdk::project::archive;
        require(pathMatchesPattern("node_modules/pkg/index.js", "**/node_modules/**"), "root ** glob failed");
        require(pathMatchesPattern("src/build/generated.cpp", "**/build/**"), "nested ** glob failed");
        require(pathMatchesPattern("cache/a.pyc", "**/*.py?"), "question-mark glob failed");
        require(!pathMatchesPattern("src/output.cpp", "**/out/**"), "non-match glob was accepted");
        require(!isValidExcludePattern("../secret"), "parent traversal glob was accepted");
        require(!isValidExcludePattern("C:\\secret\\**"), "absolute Windows glob was accepted");
        require(!isValidExcludePattern("!keep.txt"), "negative glob was accepted");
        require(sanitizeArchiveStem("CON") == "_CON", "reserved Windows file name was not sanitized");
        require(sanitizeArchiveStem("CON.txt") == "_CON.txt", "reserved Windows stem with extension was not sanitized");
        require(sanitizeArchiveStem("bad:name. ") == "bad_name", "invalid/trailing file name was not sanitized");
    }

    void testCleanSinglePackAndUnicode() {
        TemporaryDirectory temporary("clean-single");
        const auto         root   = temporary.path() / utf8Path("\xE4\xB8\xAD\xE6\x96\x87\xE9\xA1\xB9\xE7\x9B\xAE");
        const auto         output = temporary.path() / "output";
        fs::create_directories(root / utf8Path("\xE7\xA9\xBA\xE7\x9B\xAE\xE5\xBD\x95"));
        fs::create_directories(output);
        writeText(
            root / "manifest.json",
            manifest("Single", "11111111-1111-4111-8111-111111111111", "11111111-1111-4111-8111-222222222222")
        );
        writeText(root / "scripts" / utf8Path("\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8.py"), "print('ok')");
        writeText(root / "types.pyi", "typing");
        writeText(root / "scripts" / "cache.pyc", "bytecode");
        writeText(root / "work.mcscfg", "{}");
        writeText(root / ".env", "SECRET=value");
        writeText(root / ".env.local", "LOCAL_SECRET=value");
        writeText(root / ".git" / "config", "git");
        writeText(root / ".hg" / "store" / "data", "hg");
        writeText(root / ".svn" / "wc.db", "svn");
        writeText(root / ".mcdk" / "state.json", "{}");
        writeText(root / ".vscode" / "settings.json", "{}");
        writeText(root / "node_modules" / "pkg" / "index.js", "module");
        writeText(root / "nested" / "build" / "generated.obj", "build");
        writeText(root / "nested" / "out" / "generated.js", "out");
        writeText(root / "nested" / "dist" / "bundle.js", "dist");
        writeText(root / "nested" / "target" / "artifact", "target");
        writeText(root / "nested" / ".cache" / "entry", "cache");
        writeText(root / "nested" / ".pytest_cache" / "entry", "cache");
        writeText(root / "nested" / "__pycache__" / "module.cpython.pyc", "cache");
        writeText(root / "nested" / "config" / ".env.dev", "NESTED_SECRET=value");
        writeText(root / "nested" / "recovery" / ".mcdk-project-staged.tmp", "staged manifest");
        writeText(root / "nested" / "recovery" / ".mcdk-project-backup.rollback.tmp", "rollback manifest");
        writeText(root / "nested" / "recovery" / ".mcdk-project-capture.published-rollback.tmp", "published manifest");
        writeText(root / "nested" / "recovery" / ".mcdk-project-almost.tmp.keep", "keep");
        writeText(root / "src" / "building.txt", "keep");
        writeText(root / "docs" / "internal.txt", "exclude from release");

        const auto external = temporary.path() / "ExternalPack";
        writeText(
            external / "manifest.json",
            manifest("External", "19999999-9999-4999-8999-111111111111", "19999999-9999-4999-8999-222222222222")
        );
        writeText(external / "external-only.txt", "must not export");
        writeText(
            root / ".mcdev.json",
            R"({"included_mod_dirs":["../ExternalPack"],"export_options":{"clean_exclude_patterns":["docs/**"]}})"
        );

        const auto result =
            mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Rename});
        require(result.archivePath.has_value(), "clean export returned no archive path");
        require(
            result.archivePath->filename() == utf8Path("\xE4\xB8\xAD\xE6\x96\x87\xE9\xA1\xB9\xE7\x9B\xAE.zip"),
            "clean export file name did not use the project directory"
        );
        const auto entries = zipEntries(*result.archivePath);
        require(contains(entries, "manifest.json"), "single-pack manifest was not placed at ZIP root");
        require(contains(entries, "scripts/\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8.py"), "UTF-8 source entry is missing");
        require(contains(entries, "\xE7\xA9\xBA\xE7\x9B\xAE\xE5\xBD\x95/"), "empty directory entry is missing");
        require(!contains(entries, "types.pyi"), ".pyi artifact leaked into clean ZIP");
        require(!contains(entries, "scripts/cache.pyc"), ".pyc artifact leaked into clean ZIP");
        require(!contains(entries, "work.mcscfg"), "root development config leaked into clean ZIP");
        require(!contains(entries, ".mcdev.json"), ".mcdev.json leaked into clean ZIP");
        require(
            !contains(entries, "nested/recovery/.mcdk-project-staged.tmp"),
            "staged project temporary file leaked into clean ZIP"
        );
        require(
            !contains(entries, "nested/recovery/.mcdk-project-backup.rollback.tmp"),
            "rollback project temporary file leaked into clean ZIP"
        );
        require(
            !contains(entries, "nested/recovery/.mcdk-project-capture.published-rollback.tmp"),
            "published rollback capture leaked into clean ZIP"
        );
        require(
            contains(entries, "nested/recovery/.mcdk-project-almost.tmp.keep"),
            "similarly named legitimate clean file was excluded"
        );
        for (const auto forbidden : {
                 ".env",
                 ".env.local",
                 ".git/",
                 ".hg/",
                 ".svn/",
                 ".mcdk/",
                 ".vscode/",
                 "node_modules/",
                 "nested/build/",
                 "nested/out/",
                 "nested/dist/",
                 "nested/target/",
                 "nested/.cache/",
                 "nested/.pytest_cache/",
                 "nested/__pycache__/",
                 "nested/config/.env.dev",
             }) {
            require(
                !contains(entries, forbidden),
                "development content leaked into clean ZIP: " + std::string(forbidden)
            );
        }
        require(contains(entries, "src/building.txt"), "release filter excluded an unrelated similarly named file");
        require(!contains(entries, "docs/internal.txt"), "custom single-pack clean exclude failed");
        require(!contains(entries, "external-only.txt"), "external included_mod_dirs content leaked into clean ZIP");
        require(
            std::ranges::any_of(
                result.warnings,
                [](const std::string& warning) {
                    return warning.find("Ignored external included_mod_dirs") != std::string::npos;
                }
            ),
            "external included_mod_dirs did not produce a warning"
        );
        require(
            std::ranges::all_of(entries, [](const ZipEntry& entry) { return (entry.flag & (1UL << 11U)) != 0; }),
            "UTF-8 ZIP flag is missing"
        );
    }

    void testCleanMultiPackAndMap() {
        TemporaryDirectory temporary("clean-structures");
        const auto         output = temporary.path() / "output";
        fs::create_directories(output);

        const auto addon = temporary.path() / "MultiAddon";
        writeText(
            addon / "BP" / "manifest.json",
            manifest("BP", "22222222-2222-4222-8222-111111111111", "22222222-2222-4222-8222-222222222222")
        );
        writeText(addon / "BP" / "scripts" / "server.py", "server");
        writeText(
            addon / "RP" / "manifest.json",
            manifest("RP", "33333333-3333-4333-8333-111111111111", "33333333-3333-4333-8333-222222222222", "resources")
        );
        writeText(addon / "RP" / "textures" / "item.png", "png");
        writeText(addon / ".mcdev.json", R"({"export_options":{"clean_exclude_patterns":["BP/scripts/**"]}})");
        const auto addonResult =
            mcdk::project::exportProject(ExportRequest{addon, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto addonEntries = zipEntries(*addonResult.archivePath);
        require(contains(addonEntries, "BP/manifest.json"), "BP top-level directory is missing");
        require(contains(addonEntries, "RP/manifest.json"), "RP top-level directory is missing");
        require(!contains(addonEntries, "manifest.json"), "multi-pack manifest was flattened");
        require(!contains(addonEntries, "BP/scripts/server.py"), "multi-pack clean glob did not use ZIP-relative path");
        require(contains(addonEntries, "RP/textures/item.png"), "clean glob excluded an unrelated pack path");

        const auto colliding = temporary.path() / "CollidingAddon";
        writeText(
            colliding / "behavior_packs" / "Same" / "manifest.json",
            manifest("Same BP", "29999999-9999-4999-8999-111111111111", "29999999-9999-4999-8999-222222222222")
        );
        writeText(colliding / "behavior_packs" / "Same" / "scripts" / "server.py", "server");
        writeText(
            colliding / "resource_packs" / "Same" / "manifest.json",
            manifest(
                "Same RP",
                "39999999-9999-4999-8999-111111111111",
                "39999999-9999-4999-8999-222222222222",
                "resources"
            )
        );
        writeText(colliding / "resource_packs" / "Same" / "textures" / "item.png", "png");
        const auto collidingResult =
            mcdk::project::exportProject(ExportRequest{colliding, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto collidingEntries = zipEntries(*collidingResult.archivePath);
        require(contains(collidingEntries, "Same (BP)/manifest.json"), "colliding BP prefix was not disambiguated");
        require(contains(collidingEntries, "Same (RP)/manifest.json"), "colliding RP prefix was not disambiguated");
        require(
            contains(collidingEntries, "Same (BP)/scripts/server.py"),
            "colliding BP content used the wrong prefix"
        );
        require(
            contains(collidingEntries, "Same (RP)/textures/item.png"),
            "colliding RP content used the wrong prefix"
        );
        require(!contains(collidingEntries, "behavior_packs/"), "behavior_packs container leaked into ZIP root");
        require(!contains(collidingEntries, "resource_packs/"), "resource_packs container leaked into ZIP root");
        require(!contains(collidingEntries, "Same/manifest.json"), "colliding pack prefixes were left ambiguous");

        const auto map = temporary.path() / "MapProject";
        writeText(map / "level.dat", "level");
        writeText(map / "db" / "chunk.bin", "chunk");
        writeText(map / "db" / "private" / "debug.bin", "debug");
        writeText(map / ".private" / "secret", "secret");
        writeText(map / "build" / "generated.bin", "build");
        writeText(map / "node_modules" / "pkg" / "index.js", "module");
        writeText(map / "cache" / "module.pyc", "bytecode");
        writeText(map / "types.pyi", "typing");
        writeText(map / "db" / ".mcdk-project-map.rollback.tmp", "stale manifest");
        writeText(map / "db" / "mcdk-project-map.rollback.tmp", "keep");
        writeText(map / "studio.json", "{}");
        writeText(map / "work.mcscfg", "{}");
        writeText(
            map / "behavior_packs" / "WorldBP" / "manifest.json",
            manifest("WorldBP", "44444444-4444-4444-8444-111111111111", "44444444-4444-4444-8444-222222222222")
        );
        writeText(map / "world_behavior_packs.json", "[]");
        writeText(map / ".mcdev.json", R"({"export_options":{"clean_exclude_patterns":["db/private/**"]}})");
        const auto mapResult =
            mcdk::project::exportProject(ExportRequest{map, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto mapEntries = zipEntries(*mapResult.archivePath);
        require(contains(mapEntries, "level.dat"), "map level.dat is missing");
        require(contains(mapEntries, "db/chunk.bin"), "map database content is missing");
        require(!contains(mapEntries, "db/private/debug.bin"), "custom clean map exclude failed");
        require(contains(mapEntries, "behavior_packs/WorldBP/manifest.json"), "map pack hierarchy is missing");
        require(!contains(mapEntries, ".private/"), "dot directory leaked into clean map");
        require(!contains(mapEntries, "build/"), "build directory leaked into clean map");
        require(!contains(mapEntries, "node_modules/"), "node_modules leaked into clean map");
        require(!contains(mapEntries, "cache/module.pyc"), "Python bytecode leaked into clean map");
        require(!contains(mapEntries, "types.pyi"), "Python typing artifact leaked into clean map");
        require(
            !contains(mapEntries, "db/.mcdk-project-map.rollback.tmp"),
            "project temporary file leaked into clean map"
        );
        require(
            contains(mapEntries, "db/mcdk-project-map.rollback.tmp"),
            "similarly named legitimate map file was excluded"
        );
        require(!contains(mapEntries, "studio.json"), "studio.json leaked into clean map");
        require(!contains(mapEntries, "work.mcscfg"), "work.mcscfg leaked into clean map");
    }

    void testCleanTopLevelDirectoryExcludes() {
        TemporaryDirectory temporary("clean-top-level-excludes");
        const auto         output = temporary.path() / "output";
        fs::create_directories(output);

        const auto addon = temporary.path() / "MultiAddon";
        writeText(
            addon / "BP" / "manifest.json",
            manifest("BP", "a1111111-1111-4111-8111-111111111111", "a1111111-1111-4111-8111-222222222222")
        );
        writeText(addon / "BP" / "scripts" / "server.py", "server");
        writeText(
            addon / "RP" / "manifest.json",
            manifest(
                "RP",
                "a2222222-2222-4222-8222-111111111111",
                "a2222222-2222-4222-8222-222222222222",
                "resources"
            )
        );
        writeText(addon / ".mcdev.json", R"({"export_options":{"clean_exclude_patterns":["BP"]}})");
        const auto addonResult =
            mcdk::project::exportProject(ExportRequest{addon, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto addonEntries = zipEntries(*addonResult.archivePath);
        require(!contains(addonEntries, "BP/"), "top-level BP directory exclusion left an empty entry");
        require(!contains(addonEntries, "BP/manifest.json"), "top-level BP directory exclusion was ignored");
        require(contains(addonEntries, "RP/manifest.json"), "top-level BP exclusion removed the RP");

        const auto colliding = temporary.path() / "CollidingAddon";
        writeText(
            colliding / "behavior_packs" / "Same" / "manifest.json",
            manifest(
                "Same BP",
                "a3333333-3333-4333-8333-111111111111",
                "a3333333-3333-4333-8333-222222222222"
            )
        );
        writeText(
            colliding / "resource_packs" / "Same" / "manifest.json",
            manifest(
                "Same RP",
                "a4444444-4444-4444-8444-111111111111",
                "a4444444-4444-4444-8444-222222222222",
                "resources"
            )
        );
        writeText(
            colliding / ".mcdev.json",
            R"json({"export_options":{"clean_exclude_patterns":["Same (BP)"]}})json"
        );
        const auto collidingResult =
            mcdk::project::exportProject(ExportRequest{colliding, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto collidingEntries = zipEntries(*collidingResult.archivePath);
        require(
            !contains(collidingEntries, "Same (BP)/manifest.json"),
            "disambiguated top-level BP exclusion was ignored"
        );
        require(
            contains(collidingEntries, "Same (RP)/manifest.json"),
            "disambiguated BP exclusion removed the colliding RP"
        );

        const auto mapRoot = temporary.path() / "MapWithExternalPack";
        const auto world   = mapRoot / "world";
        writeText(world / "level.dat", "level");
        writeText(world / "world_behavior_packs.json", "[]");
        writeText(
            mapRoot / "external" / "ExternalBP" / "manifest.json",
            manifest(
                "External BP",
                "a5555555-5555-4555-8555-111111111111",
                "a5555555-5555-4555-8555-222222222222"
            )
        );
        writeText(mapRoot / "external" / "ExternalBP" / "scripts" / "server.py", "server");
        writeText(
            mapRoot / ".mcdev.json",
            R"({
  "included_mod_dirs": ["./external/ExternalBP"],
  "world_source_path": "./world",
  "export_options": {"clean_exclude_patterns": ["behavior_packs/ExternalBP"]}
})"
        );
        const auto mapResult =
            mcdk::project::exportProject(ExportRequest{mapRoot, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto mapEntries = zipEntries(*mapResult.archivePath);
        require(contains(mapEntries, "level.dat"), "external-pack exclusion removed the map root");
        require(
            !contains(mapEntries, "behavior_packs/ExternalBP/manifest.json"),
            "map external pack top-level exclusion was ignored"
        );
    }

    void testCleanMapIncludesWorkspaceExternalPacks() {
        TemporaryDirectory temporary("map-external-packs");
        const auto         root   = temporary.path() / "MapExternalPacks";
        const auto         world  = root / "world";
        const auto         output = temporary.path() / "output";
        fs::create_directories(output);

        writeText(world / "level.dat", "level");
        writeText(world / "world_behavior_packs.json", "[]");
        writeText(world / "db" / "chunk.bin", "world data");
        writeText(
            world / "behavior_packs" / "Same" / "manifest.json",
            manifest("Embedded Same BP", "41111111-1111-4111-8111-111111111111", "41111111-1111-4111-8111-222222222222")
        );
        writeText(world / "behavior_packs" / "Same" / "embedded.txt", "embedded behavior");
        writeText(
            world / "resource_packs" / "EmbeddedRP" / "manifest.json",
            manifest(
                "Embedded RP",
                "42222222-2222-4222-8222-111111111111",
                "42222222-2222-4222-8222-222222222222",
                "resources"
            )
        );
        writeText(world / "resource_packs" / "EmbeddedRP" / "embedded.txt", "embedded resource");

        writeText(
            root / "external_one" / "Same" / "manifest.json",
            manifest("External BP One", "43333333-3333-4333-8333-111111111111", "43333333-3333-4333-8333-222222222222")
        );
        writeText(root / "external_one" / "Same" / "one.txt", "external behavior one");
        writeText(
            root / "external_two" / "Same" / "manifest.json",
            manifest("External BP Two", "44444444-5555-4444-8444-111111111111", "44444444-5555-4444-8444-222222222222")
        );
        writeText(root / "external_two" / "Same" / "two.txt", "external behavior two");
        writeText(
            root / "external_resource" / "Same" / "manifest.json",
            manifest(
                "External RP",
                "45555555-5555-4555-8555-111111111111",
                "45555555-5555-4555-8555-222222222222",
                "resources"
            )
        );
        writeText(root / "external_resource" / "Same" / "resource.txt", "external resource");
        writeText(
            root / "external_unknown" / "Mystery" / "manifest.json",
            R"({
  "format_version": 2,
  "header": {
    "name": "Mixed Unknown Pack",
    "description": "archive test",
    "uuid": "46666666-6666-4666-8666-111111111111",
    "version": [1, 0, 0],
    "min_engine_version": [1, 20, 0]
  },
  "modules": [
    {
      "type": "resources",
      "uuid": "46666666-6666-4666-8666-222222222222",
      "version": [1, 0, 0]
    },
    {
      "type": "script",
      "uuid": "46666666-6666-4666-8666-333333333333",
      "version": [1, 0, 0]
    }
  ]
})"
        );
        writeText(root / "external_unknown" / "Mystery" / "unknown.txt", "unknown pack");
        writeText(
            root / ".mcdev.json",
            R"({
  "included_mod_dirs": [
    "./external_one/Same",
    "./external_two/Same",
    "./external_resource/Same",
    "./external_unknown/Mystery"
  ],
  "world_source_path": "./world"
})"
        );

        const auto result =
            mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Rename});
        const auto entries = zipEntries(*result.archivePath);
        require(contains(entries, "level.dat"), "subdirectory world was not placed at ZIP root");
        require(contains(entries, "db/chunk.bin"), "valid subdirectory world data is missing");
        require(!contains(entries, "world/level.dat"), "world source directory wrapper leaked into ZIP");
        require(contains(entries, "behavior_packs/Same/embedded.txt"), "embedded world behavior pack is missing");
        require(
            contains(entries, "behavior_packs/Same (2)/one.txt"),
            "first colliding external behavior pack used the wrong suffix"
        );
        require(
            contains(entries, "behavior_packs/Same (3)/two.txt"),
            "second colliding external behavior pack used the wrong suffix"
        );
        require(
            contains(entries, "resource_packs/Same/resource.txt"),
            "external resource pack was not mapped to resource_packs"
        );
        require(contains(entries, "resource_packs/EmbeddedRP/embedded.txt"), "embedded world resource pack is missing");
        require(contains(entries, "unknown_packs/Mystery/unknown.txt"), "unknown external pack was silently dropped");
        require(
            std::ranges::count_if(
                entries,
                [](const ZipEntry& entry) { return entry.name == "behavior_packs/Same/manifest.json"; }
            ) == 1,
            "embedded world pack was duplicated"
        );
        require(
            std::ranges::count_if(
                entries,
                [](const ZipEntry& entry) { return entry.name == "resource_packs/EmbeddedRP/manifest.json"; }
            ) == 1,
            "embedded world resource pack was duplicated"
        );
        require(
            std::ranges::any_of(
                result.warnings,
                [](const std::string& warning) { return warning.find("Pack kind is unknown") != std::string::npos; }
            ),
            "unknown external pack did not produce a warning"
        );
    }

    void testFullExcludesAndDisabledDefaults() {
        TemporaryDirectory temporary("full-excludes");
        const auto         output = temporary.path() / "output";
        fs::create_directories(output);

        const auto root = temporary.path() / "FullProject";
        writeText(
            root / "manifest.json",
            manifest("Full", "55555555-5555-4555-8555-111111111111", "55555555-5555-4555-8555-222222222222")
        );
        writeText(root / ".vscode" / "settings.json", "{}");
        writeText(root / ".git" / "config", "git");
        writeText(root / "vendor" / "library" / ".git" / "config", "nested git");
        writeText(root / "vendor" / "library" / ".hg" / "store" / "data", "nested hg");
        writeText(root / "vendor" / "library" / ".svn" / "wc.db", "nested svn");
        writeText(root / "node_modules" / "pkg" / "index.js", "module");
        writeText(root / "src" / "main.cpp", "source");
        writeText(root / "generated" / "ignored.txt", "generated");
        writeText(
            root / ".mcdev.json",
            R"({
  "export_options": {
    "use_default_full_excludes": true,
    "full_exclude_patterns": ["generated/**"]
  }
})"
        );
        const auto result =
            mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Full, ConflictPolicy::Rename});
        const auto entries = zipEntries(*result.archivePath);
        require(contains(entries, ".mcdev.json"), "full ZIP did not preserve .mcdev.json");
        require(contains(entries, ".vscode/settings.json"), "full ZIP did not preserve .vscode");
        require(contains(entries, "src/main.cpp"), "full ZIP did not preserve source");
        require(!contains(entries, ".git/config"), "default VCS exclude failed");
        require(!contains(entries, "vendor/library/.git/config"), "nested default .git exclude failed");
        require(!contains(entries, "vendor/library/.hg/store/data"), "nested default .hg exclude failed");
        require(!contains(entries, "vendor/library/.svn/wc.db"), "nested default .svn exclude failed");
        require(!contains(entries, "node_modules/pkg/index.js"), "default node_modules exclude failed");
        require(!contains(entries, "generated/ignored.txt"), "custom full exclude failed");

        const auto noDefaults = temporary.path() / "NoDefaults";
        writeText(
            noDefaults / "manifest.json",
            manifest("NoDefaults", "66666666-6666-4666-8666-111111111111", "66666666-6666-4666-8666-222222222222")
        );
        writeText(noDefaults / ".git" / "config", "git");
        writeText(noDefaults / "vendor" / ".git" / "config", "nested git");
        writeText(noDefaults / "ignored.tmp", "tmp");
        writeText(noDefaults / "nested" / "recovery" / ".mcdk-project-old.rollback.tmp", "stale manifest");
        writeText(noDefaults / "nested" / "recovery" / "mcdk-project-old.rollback.tmp", "keep");
        writeText(noDefaults / "nested" / "recovery" / ".mcdk-project-old.rollback.temp", "keep");
        writeText(
            noDefaults / ".mcdev.json",
            R"({"export_options":{"full_exclude_patterns":["*.tmp"]}})"
        );
        const auto noDefaultResult =
            mcdk::project::exportProject(ExportRequest{noDefaults, output, ExportMode::Full, ConflictPolicy::Rename});
        const auto noDefaultEntries = zipEntries(*noDefaultResult.archivePath);
        require(
            !noDefaultResult.project.exportOptions.useDefaultFullExcludes,
            "a directly configured full exclusion list still enabled hidden defaults"
        );
        require(contains(noDefaultEntries, ".git/config"), "direct full exclusion rules retained hidden defaults");
        require(
            contains(noDefaultEntries, "vendor/.git/config"),
            "direct full exclusion rules still removed nested VCS content"
        );
        require(!contains(noDefaultEntries, "ignored.tmp"), "custom-only exclude did not take effect");
        require(
            !contains(noDefaultEntries, "nested/recovery/.mcdk-project-old.rollback.tmp"),
            "project temporary file leaked into full ZIP with defaults disabled"
        );
        require(
            contains(noDefaultEntries, "nested/recovery/mcdk-project-old.rollback.tmp"),
            "full export excluded a legitimate file without the safety prefix"
        );
        require(
            contains(noDefaultEntries, "nested/recovery/.mcdk-project-old.rollback.temp"),
            "full export excluded a legitimate file without the safety suffix"
        );
    }

    void testConflictsSelfExclusionAndCleanup() {
        TemporaryDirectory temporary("conflicts");
        const auto         root   = temporary.path() / "ConflictProject";
        const auto         output = temporary.path() / "output";
        fs::create_directories(output);
        writeText(
            root / "manifest.json",
            manifest("Conflict", "77777777-7777-4777-8777-111111111111", "77777777-7777-4777-8777-222222222222")
        );
        writeText(root / "value.txt", "one");

        const auto first =
            mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Rename});
        bool destinationError = false;
        try {
            (void)mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Error});
        } catch (const ProjectError& error) {
            destinationError = error.code() == ProjectErrorCode::DestinationExists;
        }
        require(destinationError, "error conflict policy did not report destination_exists");

        const auto renamed =
            mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Rename});
        require(renamed.archivePath->filename() == "ConflictProject (2).zip", "rename policy used wrong suffix");

        writeText(root / "value.txt", "two");
        const auto overwritten =
            mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Overwrite});
        require(overwritten.archivePath == first.archivePath, "overwrite changed the requested archive path");
        require(zipEntryContent(*overwritten.archivePath, "value.txt") == "two", "overwrite did not publish new ZIP");

        (void)mcdk::project::exportProject(ExportRequest{root, root, ExportMode::Full, ConflictPolicy::Overwrite});
        const auto inside =
            mcdk::project::exportProject(ExportRequest{root, root, ExportMode::Full, ConflictPolicy::Overwrite});
        const auto insideEntries = zipEntries(*inside.archivePath);
        require(!contains(insideEntries, "ConflictProject \xE5\xAE\x8C\xE6\x95\xB4.zip"), "full ZIP included itself");
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = entry.path().filename().string();
            require(!name.starts_with(".mcdk-export-") || !name.ends_with(".tmp"), "temporary ZIP was not cleaned up");
        }
    }

    void testDestinationSubtreeIsExcluded() {
        TemporaryDirectory temporary("destination-subtree");
        const auto         root        = temporary.path() / "DestinationProject";
        const auto         destination = root / "release";
        fs::create_directories(destination);
        writeText(
            root / "manifest.json",
            manifest("Destination", "79999999-9999-4999-8999-111111111111", "79999999-9999-4999-8999-222222222222")
        );
        writeText(root / "scripts" / "server.py", "server");
        writeText(destination / "previous" / "internal-notes.txt", "private release notes");
        writeText(destination / "previous.zip", "old ZIP payload");
        writeText(root / "release-notes" / "keep.txt", "legitimate sibling");

        const auto clean =
            mcdk::project::exportProject(ExportRequest{root, destination, ExportMode::Clean, ConflictPolicy::Rename});
        const auto cleanEntries = zipEntries(*clean.archivePath);
        require(!contains(cleanEntries, "release/"), "clean ZIP included its destination directory");
        require(
            !contains(cleanEntries, "release/previous/internal-notes.txt"),
            "clean ZIP included a destination subtree file"
        );
        require(!contains(cleanEntries, "release/previous.zip"), "clean ZIP included an older destination ZIP");
        require(
            contains(cleanEntries, "release-notes/keep.txt"),
            "clean destination exclusion matched a same-prefix sibling"
        );

        const auto full =
            mcdk::project::exportProject(ExportRequest{root, destination, ExportMode::Full, ConflictPolicy::Rename});
        const auto fullEntries = zipEntries(*full.archivePath);
        require(!contains(fullEntries, "release/"), "full ZIP included its destination directory");
        require(
            !contains(fullEntries, "release/previous/internal-notes.txt"),
            "full ZIP included a destination subtree file"
        );
        require(!contains(fullEntries, "release/previous.zip"), "full ZIP included an older destination ZIP");
        require(
            contains(fullEntries, "release-notes/keep.txt"),
            "full destination exclusion matched a same-prefix sibling"
        );
    }

    void testProjectArchiveFamiliesAreExcludedAcrossDestinations() {
        TemporaryDirectory temporary("archive-family-switch");
        const auto         root         = temporary.path() / "SwitchDestinationProject";
        const auto         destinationA = root / "A";
        const auto         destinationB = root / "B";
        fs::create_directories(destinationA);
        fs::create_directories(destinationB);
        writeText(
            root / "manifest.json",
            manifest(
                "Switch Destination",
                "79999999-8888-4999-8999-111111111111",
                "79999999-8888-4999-8999-222222222222"
            )
        );
        writeText(root / "scripts" / "server.py", "server");

        (void)mcdk::project::exportProject(ExportRequest{root, destinationA, ExportMode::Clean, ConflictPolicy::Rename}
        );
        (void)mcdk::project::exportProject(ExportRequest{root, destinationA, ExportMode::Clean, ConflictPolicy::Rename}
        );
        (void)mcdk::project::exportProject(ExportRequest{root, destinationA, ExportMode::Full, ConflictPolicy::Rename});
        (void)mcdk::project::exportProject(ExportRequest{root, destinationA, ExportMode::Full, ConflictPolicy::Rename});
        writeText(destinationA / "OtherProject.zip", "unrelated archive");

        const auto verify = [](const std::vector<ZipEntry>& entries, std::string_view mode) {
            for (const auto oldArchive : {
                     "A/SwitchDestinationProject.zip",
                     "A/SwitchDestinationProject (2).zip",
                     "A/SwitchDestinationProject \xE5\xAE\x8C\xE6\x95\xB4.zip",
                     "A/SwitchDestinationProject \xE5\xAE\x8C\xE6\x95\xB4 (2).zip",
                 }) {
                require(
                    !contains(entries, oldArchive),
                    std::string(mode) + " ZIP included an old current-project archive: " + oldArchive
                );
            }
            require(
                contains(entries, "A/OtherProject.zip"),
                std::string(mode) + " ZIP excluded another project's archive"
            );
        };

        const auto clean =
            mcdk::project::exportProject(ExportRequest{root, destinationB, ExportMode::Clean, ConflictPolicy::Rename});
        verify(zipEntries(*clean.archivePath), "clean");

        const auto full =
            mcdk::project::exportProject(ExportRequest{root, destinationB, ExportMode::Full, ConflictPolicy::Rename});
        verify(zipEntries(*full.archivePath), "full");
    }

    void testSeparateConfigRootControlsTargetExport() {
        TemporaryDirectory temporary("separate-config-root");
        const auto         configRoot = temporary.path() / "workspace";
        const auto         target     = temporary.path() / "external-target";
        const auto         output     = temporary.path() / "output";
        fs::create_directories(output);
        writeText(
            configRoot / ".mcdev.json",
            R"({"included_mod_dirs":42,"export_options":{"clean_exclude_patterns":["private/**"],"use_default_full_excludes":false,"full_exclude_patterns":["private/**"]}})"
        );
        writeText(configRoot / "manifest.json", "{not a valid manifest");
        writeText(
            target / "manifest.json",
            manifest(
                "Config Root Target",
                "81111111-1111-4111-8111-111111111111",
                "82222222-2222-4222-8222-222222222222"
            )
        );
        writeText(target / "src" / "keep.txt", "keep");
        writeText(target / "private" / "drop.txt", "drop");

        const auto result = mcdk::project::exportProject(
            ExportRequest{target, output, ExportMode::Full, ConflictPolicy::Rename, configRoot}
        );
        require(
            result.project.root == fs::canonical(target),
            "Separate config root replaced the exported project root."
        );
        require(
            !result.project.exportOptions.useDefaultFullExcludes,
            "Separate config root did not supply export options."
        );
        const auto entries = zipEntries(*result.archivePath);
        require(contains(entries, "src/keep.txt"), "Target content was not exported with a separate config root.");
        require(!contains(entries, "private/drop.txt"), "Separate config root exclude pattern was not applied.");
        require(!contains(entries, ".mcdev.json"), "Workspace configuration leaked into the target archive.");

        const auto cleanResult = mcdk::project::exportProject(
            ExportRequest{target, output, ExportMode::Clean, ConflictPolicy::Rename, configRoot}
        );
        const auto cleanEntries = zipEntries(*cleanResult.archivePath);
        require(contains(cleanEntries, "src/keep.txt"), "Clean target content was not exported with a config root.");
        require(
            !contains(cleanEntries, "private/drop.txt"),
            "Separate config root clean exclude pattern was not applied."
        );
    }

    void testInvalidPatternsCreateNothing() {
        TemporaryDirectory temporary("invalid-pattern");
        const auto         root   = temporary.path() / "InvalidPattern";
        const auto         output = temporary.path() / "output";
        fs::create_directories(output);
        writeText(
            root / "manifest.json",
            manifest("InvalidPattern", "88888888-8888-4888-8888-111111111111", "88888888-8888-4888-8888-222222222222")
        );
        writeText(
            root / ".mcdev.json",
            R"({"export_options":{"clean_exclude_patterns":["../clean-outside"],"full_exclude_patterns":["../outside"]}})"
        );

        bool invalidPattern = false;
        try {
            (void)mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Full, ConflictPolicy::Rename});
        } catch (const ProjectError& error) {
            invalidPattern = error.code() == ProjectErrorCode::InvalidExportPattern;
        }
        require(invalidPattern, "invalid export pattern did not use stable error code");
        require(fs::directory_iterator(output) == fs::directory_iterator(), "invalid pattern left output files");

        invalidPattern = false;
        try {
            (void)mcdk::project::exportProject(ExportRequest{root, output, ExportMode::Clean, ConflictPolicy::Rename});
        } catch (const ProjectError& error) {
            invalidPattern = error.code() == ProjectErrorCode::InvalidExportPattern;
        }
        require(invalidPattern, "invalid clean export pattern did not use stable error code");
        require(fs::directory_iterator(output) == fs::directory_iterator(), "invalid clean pattern left output files");
    }

    void testUserConfigParsesExportOptions() {
        const auto config = mcdk::parseUserConfig(R"({
            "export_options": {
                "clean_exclude_patterns": ["drafts/**"],
                "full_exclude_patterns": ["private/**", "*.cache"]
            }
        })");
        require(!config.exportOptions.useDefaultFullExcludes, "UserConfig export default flag is incorrect");
        require(config.exportOptions.cleanExcludePatterns.size() == 1, "UserConfig clean patterns are incorrect");
        require(config.exportOptions.fullExcludePatterns.size() == 2, "UserConfig export patterns are incorrect");
    }

} // namespace

int main() {
    try {
        testGlobAndFileNameRules();
        testCleanSinglePackAndUnicode();
        testCleanMultiPackAndMap();
        testCleanTopLevelDirectoryExcludes();
        testCleanMapIncludesWorkspaceExternalPacks();
        testFullExcludesAndDisabledDefaults();
        testConflictsSelfExclusionAndCleanup();
        testDestinationSubtreeIsExcluded();
        testProjectArchiveFamiliesAreExcludedAcrossDestinations();
        testSeparateConfigRootControlsTargetExport();
        testInvalidPatternsCreateNothing();
        testUserConfigParsesExportOptions();
        std::cout << "project archive tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project archive tests failed: " << error.what() << '\n';
        return 1;
    }
}
