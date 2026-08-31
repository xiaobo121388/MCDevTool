#include <mcdk/project_archive.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <zip.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <iowin32.h>
#else
#include <unistd.h>
#endif

namespace mcdk::project {
    namespace {

        namespace fs = std::filesystem;

        constexpr std::uintmax_t zip32SizeLimit = 0xffffffffULL;
        constexpr uLong          utf8NameFlag   = 1UL << 11U;

        struct SourceRoot {
            fs::path diskRoot;
            fs::path archivePrefix;
        };

        struct ArchiveEntry {
            fs::path       diskPath;
            std::string    archiveName;
            std::uintmax_t size        = 0;
            bool           isDirectory = false;
        };

        enum class FilterKind : std::uint8_t {
            CleanAddon,
            CleanMap,
            Full,
        };

        struct CollectionContext {
            FilterKind                      filterKind;
            const std::vector<std::string>* excludePatterns = nullptr;
            fs::path                        destination;
            std::optional<fs::path>         excludedDestinationSubtree;
            std::string                     projectArchiveStem;
            std::vector<std::string>*       warnings = nullptr;
            std::vector<ArchiveEntry>       entries;
            std::set<std::string>           entryNames;
        };

        [[nodiscard]] std::string pathToUtf8(const fs::path& path) {
            const auto value = path.generic_u8string();
            return {reinterpret_cast<const char*>(value.data()), value.size()};
        }

        [[nodiscard]] fs::path pathFromUtf8(std::string_view value) {
            return fs::path(std::u8string(
                reinterpret_cast<const char8_t*>(value.data()),
                reinterpret_cast<const char8_t*>(value.data() + value.size())
            ));
        }

        [[nodiscard]] std::string asciiLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        [[nodiscard]] bool asciiEqualIgnoreCase(std::string_view left, std::string_view right) noexcept {
            if (left.size() != right.size()) {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index) {
                const auto leftCharacter  = static_cast<unsigned char>(left[index]);
                const auto rightCharacter = static_cast<unsigned char>(right[index]);
                if (std::tolower(leftCharacter) != std::tolower(rightCharacter)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool canonicalPathComponentEqual(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
            return asciiEqualIgnoreCase(pathToUtf8(left), pathToUtf8(right));
#else
            return left == right;
#endif
        }

        [[nodiscard]] bool isCanonicalPathWithin(const fs::path& path, const fs::path& root) {
            const auto normalizedPath = path.lexically_normal();
            const auto normalizedRoot = root.lexically_normal();
            auto       pathPart       = normalizedPath.begin();
            for (auto rootPart = normalizedRoot.begin(); rootPart != normalizedRoot.end(); ++rootPart, ++pathPart) {
                if (pathPart == normalizedPath.end() || !canonicalPathComponentEqual(*pathPart, *rootPart)) {
                    return false;
                }
            }
            return true;
        }

        [[noreturn]] void throwIoError(const fs::path& path, std::string_view operation, const std::error_code& error) {
            std::string message(operation);
            message.append(": ");
            message.append(error.message());
            throw ProjectError(ProjectErrorCode::IoError, path, std::move(message));
        }

        [[noreturn]] void throwArchiveError(const fs::path& path, std::string message) {
            throw ProjectError(ProjectErrorCode::ArchiveError, path, std::move(message));
        }

        [[nodiscard]] fs::path canonicalPath(const fs::path& path) {
            std::error_code error;
            auto            result = fs::weakly_canonical(path, error);
            if (error) {
                throwIoError(path, "Unable to resolve path", error);
            }
            return result.lexically_normal();
        }

        [[nodiscard]] bool isPathWithin(const fs::path& path, const fs::path& root) {
            std::error_code error;
            const auto      relative = fs::relative(canonicalPath(path), canonicalPath(root), error);
            if (error || relative.is_absolute()) {
                return false;
            }
            for (const auto& component : relative) {
                if (component == "..") {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool pathsReferToSameLocation(const fs::path& left, const fs::path& right) {
            std::error_code error;
            const bool      equivalent = fs::equivalent(left, right, error);
            if (!error) {
                return equivalent;
            }
#ifdef _WIN32
            return asciiEqualIgnoreCase(pathToUtf8(canonicalPath(left)), pathToUtf8(canonicalPath(right)));
#else
            return canonicalPath(left) == canonicalPath(right);
#endif
        }

        [[nodiscard]] bool pathExistsNoFollow(const fs::path& path) {
            std::error_code error;
            const auto      status = fs::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory) {
                return false;
            }
            if (error) {
                throwIoError(path, "Unable to inspect path", error);
            }
            return fs::exists(status);
        }

        [[nodiscard]] bool isLinkOrReparsePoint(const fs::path& path, const fs::file_status& status) {
            if (fs::is_symlink(status)) {
                return true;
            }
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const std::error_code error(static_cast<int>(GetLastError()), std::system_category());
                throwIoError(path, "Unable to inspect Windows file attributes", error);
            }
            return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            (void)path;
            return false;
#endif
        }

        [[nodiscard]] std::string normalizeGlobText(std::string_view value) {
            std::string result;
            result.reserve(value.size());
            for (const char character : value) {
                result.push_back(character == '\\' ? '/' : character);
            }
            while (result.starts_with("./")) {
                result.erase(0, 2);
            }
            while (!result.empty() && result.back() == '/') {
                result.pop_back();
            }
            return result;
        }

        [[nodiscard]] std::vector<std::string_view> splitGlobSegments(const std::string& value) {
            std::vector<std::string_view> result;
            std::size_t                   begin = 0;
            while (begin <= value.size()) {
                const auto end  = value.find('/', begin);
                const auto size = (end == std::string::npos ? value.size() : end) - begin;
                if (size != 0) {
                    result.emplace_back(value.data() + begin, size);
                }
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
            return result;
        }

        [[nodiscard]] bool segmentMatches(std::string_view value, std::string_view pattern) noexcept {
            std::size_t valueIndex   = 0;
            std::size_t patternIndex = 0;
            std::size_t starIndex    = std::string_view::npos;
            std::size_t retryIndex   = 0;

            while (valueIndex < value.size()) {
                if (patternIndex < pattern.size()
                    && (pattern[patternIndex] == '?' || pattern[patternIndex] == value[valueIndex])) {
                    ++patternIndex;
                    ++valueIndex;
                    continue;
                }
                if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
                    starIndex  = patternIndex++;
                    retryIndex = valueIndex;
                    continue;
                }
                if (starIndex != std::string_view::npos) {
                    patternIndex = starIndex + 1;
                    valueIndex   = ++retryIndex;
                    continue;
                }
                return false;
            }
            while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
                ++patternIndex;
            }
            return patternIndex == pattern.size();
        }

        [[nodiscard]] bool
        matchGlobSegments(const std::vector<std::string_view>& path, const std::vector<std::string_view>& pattern) {
            const auto               columns = path.size() + 1;
            std::vector<std::int8_t> memo((pattern.size() + 1) * columns, -1);
            const auto match = [&](const auto& self, const std::size_t patternIndex, const std::size_t pathIndex
                               ) -> bool {
                auto& known = memo[patternIndex * columns + pathIndex];
                if (known >= 0) {
                    return known != 0;
                }

                bool result = false;
                if (patternIndex == pattern.size()) {
                    result = pathIndex == path.size();
                } else if (pattern[patternIndex] == "**") {
                    result = self(self, patternIndex + 1, pathIndex)
                          || (pathIndex < path.size() && self(self, patternIndex, pathIndex + 1));
                } else if (pathIndex < path.size() && segmentMatches(path[pathIndex], pattern[patternIndex])) {
                    result = self(self, patternIndex + 1, pathIndex + 1);
                }
                known = result ? 1 : 0;
                return result;
            };
            return match(match, 0, 0);
        }

        [[nodiscard]] bool isRootDevelopmentEntry(const fs::path& relative, bool isDirectory) {
            if (relative.parent_path() != fs::path()) {
                return false;
            }
            const auto name = asciiLower(pathToUtf8(relative.filename()));
            if (isDirectory) {
                return name == ".git" || name == ".hg" || name == ".svn" || name == ".vscode" || name == ".mcdk";
            }
            return name == ".mcdev.json" || name == ".env" || name.starts_with(".env.") || name == "studio.json"
                || name == "work.mcscfg";
        }

        [[nodiscard]] bool isPythonDevelopmentArtifact(const fs::path& relative) {
            const auto extension = asciiLower(pathToUtf8(relative.extension()));
            return extension == ".pyi" || extension == ".pyc" || extension == ".pyo";
        }

        [[nodiscard]] bool isCleanAddonSafetyExcluded(const fs::path& relative) {
            static const std::vector<std::string> patterns{
                "**/.git/**",        "**/.hg/**",         "**/.svn/**",
                "**/.mcdk/**",       "**/.vscode/**",     "**/.mcdev.json",
                "**/.env",           "**/.env.*",         "**/node_modules/**",
                "**/build/**",       "**/out/**",         "**/dist/**",
                "**/target/**",      "**/.cache/**",      "**/.pytest_cache/**",
                "**/.mypy_cache/**", "**/.ruff_cache/**", "**/__pycache__/**",
                "**/*.pyc",          "**/*.pyo",
            };
            const auto relativeName = pathToUtf8(relative);
            return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
                return archive::pathMatchesPattern(relativeName, pattern);
            });
        }

        [[nodiscard]] bool hasDotComponent(const fs::path& relative) {
            for (const auto& component : relative) {
                const auto value = pathToUtf8(component);
                if (!value.empty() && value.front() == '.') {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool isMapDevelopmentEntry(const fs::path& relative) {
            const auto name = asciiLower(pathToUtf8(relative.filename()));
            return name == ".mcdev.json" || name == "studio.json" || name == "work.mcscfg";
        }

        [[nodiscard]] bool isArchiveFamilyName(std::string_view fileName, std::string_view familyStem) {
            const std::string baseName = std::string(familyStem) + ".zip";
            if (fileName == baseName) {
                return true;
            }
            const std::string prefix = std::string(familyStem) + " (";
            if (!fileName.starts_with(prefix) || !fileName.ends_with(").zip")) {
                return false;
            }
            const auto number = fileName.substr(prefix.size(), fileName.size() - prefix.size() - 5);
            return !number.empty() && std::all_of(number.begin(), number.end(), [](const unsigned char character) {
                return std::isdigit(character) != 0;
            });
        }

        [[nodiscard]] bool isMandatorySafetyExcluded(
            const fs::path&                diskPath,
            const fs::path&                destination,
            const std::optional<fs::path>& excludedDestinationSubtree,
            std::string_view               projectArchiveStem,
            bool                           isDirectory
        ) {
            if (excludedDestinationSubtree.has_value()
                && isCanonicalPathWithin(diskPath, *excludedDestinationSubtree)) {
                return true;
            }
            const auto name = pathToUtf8(diskPath.filename());
            if (name.starts_with(".mcdk-project-") && name.ends_with(".tmp")) {
                return true;
            }
            if (!isDirectory) {
                const auto fullArchiveStem = std::string(projectArchiveStem) + " \xE5\xAE\x8C\xE6\x95\xB4";
                if (isArchiveFamilyName(name, projectArchiveStem) || isArchiveFamilyName(name, fullArchiveStem)) {
                    return true;
                }
            }
            return pathsReferToSameLocation(diskPath.parent_path(), destination) && name.starts_with(".mcdk-export-")
                && name.ends_with(".tmp");
        }

        [[nodiscard]] bool shouldExclude(
            const CollectionContext& context,
            const fs::path&          diskPath,
            const fs::path&          sourceRelative,
            const fs::path&          archiveRelative,
            bool                     isDirectory
        ) {
            if (isMandatorySafetyExcluded(
                    diskPath,
                    context.destination,
                    context.excludedDestinationSubtree,
                    context.projectArchiveStem,
                    isDirectory
                )) {
                return true;
            }
            if (context.excludePatterns != nullptr) {
                const auto relativeName = pathToUtf8(archiveRelative);
                if (std::any_of(
                        context.excludePatterns->begin(),
                        context.excludePatterns->end(),
                        [&](const std::string& pattern) { return archive::pathMatchesPattern(relativeName, pattern); }
                    )) {
                    return true;
                }
            }
            switch (context.filterKind) {
            case FilterKind::CleanAddon:
                return isRootDevelopmentEntry(sourceRelative, isDirectory)
                    || (!isDirectory && isPythonDevelopmentArtifact(sourceRelative))
                    || isCleanAddonSafetyExcluded(sourceRelative);
            case FilterKind::CleanMap:
                return hasDotComponent(sourceRelative) || isMapDevelopmentEntry(sourceRelative)
                    || (!isDirectory && isPythonDevelopmentArtifact(sourceRelative))
                    || isCleanAddonSafetyExcluded(sourceRelative);
            case FilterKind::Full:
                return false;
            }
            return false;
        }

        void addEntry(CollectionContext& context, ArchiveEntry entry) {
            if (entry.archiveName.empty()) {
                return;
            }
            if (entry.isDirectory && entry.archiveName.back() != '/') {
                entry.archiveName.push_back('/');
            }
            if (!context.entryNames.insert(entry.archiveName).second) {
                throw ProjectError(
                    ProjectErrorCode::ArchiveError,
                    entry.diskPath,
                    "Two source paths map to the same ZIP entry: " + entry.archiveName
                );
            }
            context.entries.push_back(std::move(entry));
        }

        void collectDirectory(
            CollectionContext& context,
            const SourceRoot&  source,
            const fs::path&    diskDirectory,
            const fs::path&    sourceRelative
        ) {
            std::error_code        error;
            fs::directory_iterator iterator(diskDirectory, fs::directory_options::none, error);
            if (error) {
                throwIoError(diskDirectory, "Unable to enumerate directory", error);
            }

            std::vector<fs::directory_entry> children;
            const fs::directory_iterator     end;
            while (iterator != end) {
                children.push_back(*iterator);
                iterator.increment(error);
                if (error) {
                    throwIoError(diskDirectory, "Unable to enumerate directory", error);
                }
            }
            std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
                return pathToUtf8(left.path().filename()) < pathToUtf8(right.path().filename());
            });

            for (const auto& child : children) {
                const auto relative = sourceRelative / child.path().filename();
                auto       status   = child.symlink_status(error);
                if (error) {
                    throwIoError(child.path(), "Unable to inspect archive source", error);
                }
                if (isLinkOrReparsePoint(child.path(), status)) {
                    context.warnings->push_back("Skipped link or reparse point: " + pathToUtf8(child.path()));
                    continue;
                }

                if (fs::is_directory(status)) {
                    if (shouldExclude(context, child.path(), relative, source.archivePrefix / relative, true)) {
                        continue;
                    }
                    const auto archiveName = pathToUtf8(source.archivePrefix / relative);
                    addEntry(context, ArchiveEntry{child.path(), archiveName, 0, true});
                    collectDirectory(context, source, child.path(), relative);
                    continue;
                }

                if (fs::is_regular_file(status)) {
                    if (shouldExclude(context, child.path(), relative, source.archivePrefix / relative, false)) {
                        continue;
                    }
                    const auto size = fs::file_size(child.path(), error);
                    if (error) {
                        throwIoError(child.path(), "Unable to read file size", error);
                    }
                    addEntry(
                        context,
                        ArchiveEntry{child.path(), pathToUtf8(source.archivePrefix / relative), size, false}
                    );
                    continue;
                }

                context.warnings->push_back("Skipped unsupported filesystem entry: " + pathToUtf8(child.path()));
            }
        }

        [[nodiscard]] PackKind packKindForDirectory(const ProjectSummary& summary, const fs::path& directory) {
            PackKind result = PackKind::Unknown;
            for (const auto& manifest : summary.manifests) {
                if (!pathsReferToSameLocation(manifest.packDirectory, directory)) {
                    continue;
                }
                if (result == PackKind::Unknown) {
                    result = manifest.kind;
                } else if (manifest.kind != PackKind::Unknown && result != manifest.kind) {
                    return PackKind::Unknown;
                }
            }
            if (result != PackKind::Unknown) {
                return result;
            }
            const auto parentName = asciiLower(pathToUtf8(directory.parent_path().filename()));
            if (parentName == "behavior_packs") {
                return PackKind::Behavior;
            }
            if (parentName == "resource_packs") {
                return PackKind::Resource;
            }
            return PackKind::Unknown;
        }

        [[nodiscard]] std::string packKindQualifier(PackKind kind) {
            switch (kind) {
            case PackKind::Behavior:
                return "BP";
            case PackKind::Resource:
                return "RP";
            case PackKind::Unknown:
                return "Pack";
            }
            return "Pack";
        }

        [[nodiscard]] std::vector<SourceRoot>
        cleanSourceRoots(const ProjectSummary& summary, std::vector<std::string>& warnings) {
            if (summary.kind == ProjectKind::Map) {
                if (!summary.worldDirectory.has_value()) {
                    throw ProjectError(
                        ProjectErrorCode::InvalidProject,
                        summary.root,
                        "Map discovery did not provide a world directory"
                    );
                }
                if (!isPathWithin(*summary.worldDirectory, summary.root)) {
                    throw ProjectError(
                        ProjectErrorCode::InvalidProject,
                        *summary.worldDirectory,
                        "The configured world directory is outside the project root"
                    );
                }
                const auto              worldDirectory = canonicalPath(*summary.worldDirectory);
                std::vector<SourceRoot> result{{worldDirectory, {}}};

                std::vector<fs::path> externalPackDirectories;
                for (const auto& directory : summary.packDirectories) {
                    if (!isPathWithin(directory, summary.root) || isPathWithin(directory, worldDirectory)) {
                        continue;
                    }
                    const auto canonical = canonicalPath(directory);
                    if (std::none_of(
                            externalPackDirectories.begin(),
                            externalPackDirectories.end(),
                            [&](const fs::path& existing) { return pathsReferToSameLocation(existing, canonical); }
                        )) {
                        externalPackDirectories.push_back(canonical);
                    }
                }
                std::sort(
                    externalPackDirectories.begin(),
                    externalPackDirectories.end(),
                    [](const fs::path& left, const fs::path& right) { return pathToUtf8(left) < pathToUtf8(right); }
                );

                struct ContainerNames {
                    std::string           archiveName;
                    std::set<std::string> used;
                    bool                  initialized = false;
                };
                ContainerNames behavior{"behavior_packs"};
                ContainerNames resource{"resource_packs"};
                ContainerNames unknown{"unknown_packs"};

                const auto initializeContainer = [&](ContainerNames& container) {
                    if (container.initialized) {
                        return;
                    }
                    container.initialized    = true;
                    const auto containerPath = worldDirectory / container.archiveName;
                    if (!pathExistsNoFollow(containerPath)) {
                        return;
                    }
                    std::error_code error;
                    const auto      status = fs::symlink_status(containerPath, error);
                    if (error) {
                        throwIoError(containerPath, "Unable to inspect world pack container", error);
                    }
                    if (isLinkOrReparsePoint(containerPath, status)) {
                        warnings.push_back(
                            "Ignored linked world pack container while mapping external packs: "
                            + pathToUtf8(containerPath)
                        );
                        return;
                    }
                    if (!fs::is_directory(status)) {
                        throw ProjectError(
                            ProjectErrorCode::ArchiveError,
                            containerPath,
                            "A world pack container path is not a directory"
                        );
                    }
                    fs::directory_iterator iterator(containerPath, fs::directory_options::none, error);
                    if (error) {
                        throwIoError(containerPath, "Unable to enumerate world pack container", error);
                    }
                    const fs::directory_iterator end;
                    while (iterator != end) {
                        container.used.insert(asciiLower(pathToUtf8(iterator->path().filename())));
                        iterator.increment(error);
                        if (error) {
                            throwIoError(containerPath, "Unable to enumerate world pack container", error);
                        }
                    }
                };

                for (const auto& directory : externalPackDirectories) {
                    auto* container = &unknown;
                    switch (packKindForDirectory(summary, directory)) {
                    case PackKind::Behavior:
                        container = &behavior;
                        break;
                    case PackKind::Resource:
                        container = &resource;
                        break;
                    case PackKind::Unknown:
                        warnings.push_back(
                            "Pack kind is unknown; archived under unknown_packs: " + pathToUtf8(directory)
                        );
                        break;
                    }
                    initializeContainer(*container);

                    const auto baseName = pathToUtf8(directory.filename());
                    if (baseName.empty()) {
                        throw ProjectError(
                            ProjectErrorCode::ArchiveError,
                            directory,
                            "Unable to determine the external map pack directory name"
                        );
                    }
                    auto candidate = baseName;
                    for (std::uint64_t suffix = 2; !container->used.insert(asciiLower(candidate)).second; ++suffix) {
                        candidate = baseName + " (" + std::to_string(suffix) + ")";
                    }
                    if (candidate != baseName) {
                        warnings.push_back(
                            "Renamed colliding external map pack in ZIP: " + baseName + " -> " + candidate
                        );
                    }
                    result.push_back({directory, pathFromUtf8(container->archiveName) / pathFromUtf8(candidate)});
                }
                return result;
            }

            std::vector<fs::path> packDirectories;
            for (const auto& directory : summary.packDirectories) {
                if (!isPathWithin(directory, summary.root)) {
                    continue;
                }
                const auto canonical = canonicalPath(directory);
                if (std::none_of(packDirectories.begin(), packDirectories.end(), [&](const fs::path& existing) {
                        return pathsReferToSameLocation(existing, canonical);
                    })) {
                    packDirectories.push_back(canonical);
                }
            }
            if (packDirectories.empty()) {
                throw ProjectError(
                    ProjectErrorCode::InvalidProject,
                    summary.root,
                    "No in-project pack directory is available for clean export"
                );
            }
            std::sort(packDirectories.begin(), packDirectories.end(), [](const fs::path& left, const fs::path& right) {
                return pathToUtf8(left) < pathToUtf8(right);
            });

            std::vector<SourceRoot> result;
            result.reserve(packDirectories.size());
            const bool singlePack = packDirectories.size() == 1;

            std::set<std::string> usedPrefixKeys;
            if (!singlePack) {
                for (const auto& directory : packDirectories) {
                    const auto baseName = pathToUtf8(directory.filename());
                    const auto occurrences =
                        std::count_if(packDirectories.begin(), packDirectories.end(), [&](const fs::path& candidate) {
                            return asciiEqualIgnoreCase(pathToUtf8(candidate.filename()), baseName);
                        });
                    if (occurrences == 1) {
                        usedPrefixKeys.insert(asciiLower(baseName));
                    }
                }
            }

            for (const auto& directory : packDirectories) {
                if (singlePack) {
                    result.push_back({directory, {}});
                    continue;
                }

                const auto baseName = pathToUtf8(directory.filename());
                if (baseName.empty()) {
                    throw ProjectError(
                        ProjectErrorCode::ArchiveError,
                        directory,
                        "Unable to determine the pack directory name"
                    );
                }
                const auto occurrences =
                    std::count_if(packDirectories.begin(), packDirectories.end(), [&](const fs::path& candidate) {
                        return asciiEqualIgnoreCase(pathToUtf8(candidate.filename()), baseName);
                    });
                if (occurrences == 1) {
                    result.push_back({directory, directory.filename()});
                    continue;
                }

                const auto qualifier = packKindQualifier(packKindForDirectory(summary, directory));
                auto       candidate = baseName + " (" + qualifier + ")";
                for (std::uint64_t suffix = 2; !usedPrefixKeys.insert(asciiLower(candidate)).second; ++suffix) {
                    candidate = baseName + " (" + qualifier + " " + std::to_string(suffix) + ")";
                }
                warnings.push_back("Renamed colliding pack directory in ZIP: " + baseName + " -> " + candidate);
                result.push_back({directory, pathFromUtf8(candidate)});
            }
            return result;
        }

        [[nodiscard]] std::vector<std::string>
        exportExcludePatterns(const ProjectSummary& summary, ExportMode mode) {
            std::vector<std::string> patterns;
            if (mode == ExportMode::Full) {
                if (summary.exportOptions.useDefaultFullExcludes) {
                    patterns.assign(
                        archive::defaultFullExcludePatterns().begin(),
                        archive::defaultFullExcludePatterns().end()
                    );
                }
                patterns.insert(
                    patterns.end(),
                    summary.exportOptions.fullExcludePatterns.begin(),
                    summary.exportOptions.fullExcludePatterns.end()
                );
            } else {
                patterns = summary.exportOptions.cleanExcludePatterns;
            }
            for (const auto& pattern : patterns) {
                if (!archive::isValidExcludePattern(pattern)) {
                    throw ProjectError(
                        ProjectErrorCode::InvalidExportPattern,
                        summary.root / ".mcdev.json",
                        std::string("Invalid ") + (mode == ExportMode::Full ? "full" : "clean")
                            + " export exclude pattern: " + pattern
                    );
                }
            }
            return patterns;
        }

        [[nodiscard]] std::string projectDirectoryName(const ProjectSummary& summary) {
            auto name = pathToUtf8(summary.root.filename());
            if (name.empty()) {
                name = pathToUtf8(summary.root.parent_path().filename());
            }
            return archive::sanitizeArchiveStem(name);
        }

        [[nodiscard]] fs::path
        selectRequestedArchivePath(const fs::path& destination, std::string_view familyStem, ConflictPolicy conflict) {
            const auto base = destination / pathFromUtf8(std::string(familyStem) + ".zip");
            if (conflict == ConflictPolicy::Overwrite) {
                return base;
            }
            if (!pathExistsNoFollow(base)) {
                return base;
            }
            if (conflict == ConflictPolicy::Error) {
                throw ProjectError(ProjectErrorCode::DestinationExists, base, "The destination ZIP already exists");
            }
            for (std::uint64_t suffix = 2; suffix < std::numeric_limits<std::uint64_t>::max(); ++suffix) {
                const auto candidate =
                    destination / pathFromUtf8(std::string(familyStem) + " (" + std::to_string(suffix) + ").zip");
                if (!pathExistsNoFollow(candidate)) {
                    return candidate;
                }
            }
            throw ProjectError(
                ProjectErrorCode::ArchiveError,
                destination,
                "Unable to allocate a non-conflicting archive name"
            );
        }

        [[nodiscard]] fs::path temporaryArchivePath(const fs::path& destination) {
            static std::atomic<std::uint64_t> counter{0};
            std::random_device                random;
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto serial = counter.fetch_add(1, std::memory_order_relaxed);
                const auto ticks =
                    static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                std::ostringstream name;
                name << ".mcdk-export-" << std::hex << ticks << '-' << random() << '-' << serial << ".tmp";
                const auto candidate = destination / name.str();
                if (!pathExistsNoFollow(candidate)) {
                    return candidate;
                }
            }
            throw ProjectError(ProjectErrorCode::IoError, destination, "Unable to allocate a temporary archive path");
        }

        class TemporaryArchive final {
        public:
            explicit TemporaryArchive(fs::path path) : path_(std::move(path)) {}

            TemporaryArchive(const TemporaryArchive&)            = delete;
            TemporaryArchive& operator=(const TemporaryArchive&) = delete;

            ~TemporaryArchive() {
                if (!published_) {
                    std::error_code ignored;
                    fs::remove(path_, ignored);
                }
            }

            [[nodiscard]] const fs::path& path() const noexcept { return path_; }

            void markPublished() noexcept { published_ = true; }

        private:
            fs::path path_;
            bool     published_ = false;
        };

        class ZipWriter final {
        public:
            explicit ZipWriter(const fs::path& path) : path_(path) {
#ifdef _WIN32
                zlib_filefunc64_def fileFunctions{};
                fill_win32_filefunc64W(&fileFunctions);
                handle_ = zipOpen2_64(path.c_str(), APPEND_STATUS_CREATE, nullptr, &fileFunctions);
#else
                handle_ = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
#endif
                if (handle_ == nullptr) {
                    throwArchiveError(path_, "Unable to create temporary ZIP archive");
                }
            }

            ZipWriter(const ZipWriter&)            = delete;
            ZipWriter& operator=(const ZipWriter&) = delete;

            ~ZipWriter() {
                if (handle_ != nullptr) {
                    if (entryOpen_) {
                        zipCloseFileInZip(handle_);
                    }
                    zipClose(handle_, nullptr);
                }
            }

            void addDirectory(const ArchiveEntry& entry) {
                openEntry(entry, 0, 0);
                closeEntry(entry.diskPath);
            }

            void addFile(const ArchiveEntry& entry) {
                const auto extension = asciiLower(pathToUtf8(entry.diskPath.extension()));
                static constexpr std::array<std::string_view, 18> compressedExtensions{
                    ".7z",
                    ".bz2",
                    ".gif",
                    ".gz",
                    ".jpeg",
                    ".jpg",
                    ".mcaddon",
                    ".mcpack",
                    ".mp3",
                    ".mp4",
                    ".ogg",
                    ".pdf",
                    ".png",
                    ".rar",
                    ".webp",
                    ".woff",
                    ".woff2",
                    ".zip",
                };
                const bool store = std::find(compressedExtensions.begin(), compressedExtensions.end(), extension)
                                != compressedExtensions.end();
                openEntry(entry, store ? 0 : Z_DEFLATED, store ? 0 : Z_BEST_SPEED);

                std::ifstream input(entry.diskPath, std::ios::binary);
                if (!input) {
                    throw ProjectError(ProjectErrorCode::IoError, entry.diskPath, "Unable to open archive source file");
                }
                std::array<char, 128 * 1024> buffer{};
                while (input) {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = input.gcount();
                    if (count > 0) {
                        const auto result =
                            zipWriteInFileInZip(handle_, buffer.data(), static_cast<unsigned int>(count));
                        if (result != ZIP_OK) {
                            throwArchiveError(entry.diskPath, "Unable to write file data to ZIP archive");
                        }
                    }
                }
                if (!input.eof()) {
                    throw ProjectError(ProjectErrorCode::IoError, entry.diskPath, "Unable to read archive source file");
                }
                closeEntry(entry.diskPath);
            }

            void finish() {
                const auto handle = std::exchange(handle_, nullptr);
                const auto result = zipClose(handle, nullptr);
                if (result != ZIP_OK) {
                    throwArchiveError(path_, "Unable to finalize ZIP archive");
                }
            }

        private:
            void openEntry(const ArchiveEntry& entry, int method, int level) {
                zip_fileinfo information{};
                information.external_fa = entry.isDirectory ? (0040755UL << 16U) | 0x10UL : 0100644UL << 16U;
                const auto result       = zipOpenNewFileInZip4_64(
                    handle_,
                    entry.archiveName.c_str(),
                    &information,
                    nullptr,
                    0,
                    nullptr,
                    0,
                    nullptr,
                    method,
                    level,
                    0,
                    -MAX_WBITS,
                    8,
                    Z_DEFAULT_STRATEGY,
                    nullptr,
                    0,
                    3UL << 8U,
                    utf8NameFlag,
                    entry.size >= zip32SizeLimit ? 1 : 0
                );
                if (result != ZIP_OK) {
                    throwArchiveError(entry.diskPath, "Unable to create ZIP entry: " + entry.archiveName);
                }
                entryOpen_ = true;
            }

            void closeEntry(const fs::path& path) {
                entryOpen_        = false;
                const auto result = zipCloseFileInZip(handle_);
                if (result != ZIP_OK) {
                    throwArchiveError(path, "Unable to finalize ZIP entry");
                }
            }

            fs::path path_;
            zipFile  handle_    = nullptr;
            bool     entryOpen_ = false;
        };

        void publishWithoutReplacement(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
            if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
                const auto windowsError = GetLastError();
                if (windowsError == ERROR_FILE_EXISTS || windowsError == ERROR_ALREADY_EXISTS) {
                    throw ProjectError(
                        ProjectErrorCode::DestinationExists,
                        target,
                        "The destination ZIP was created by another process"
                    );
                }
                throwIoError(
                    target,
                    "Unable to publish ZIP archive",
                    std::error_code(static_cast<int>(windowsError), std::system_category())
                );
            }
#else
            if (::link(temporary.c_str(), target.c_str()) != 0) {
                const auto linkError = errno;
                if (linkError == EEXIST) {
                    throw ProjectError(
                        ProjectErrorCode::DestinationExists,
                        target,
                        "The destination ZIP was created by another process"
                    );
                }
                throwIoError(
                    target,
                    "Unable to publish ZIP archive",
                    std::error_code(linkError, std::generic_category())
                );
            }
            if (::unlink(temporary.c_str()) != 0) {
                const auto      unlinkError = errno;
                std::error_code ignored;
                fs::remove(temporary, ignored);
                if (pathExistsNoFollow(temporary)) {
                    throwIoError(
                        temporary,
                        "Unable to remove temporary ZIP link",
                        std::error_code(unlinkError, std::generic_category())
                    );
                }
            }
#endif
        }

        void publishWithReplacement(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
            if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                == 0) {
                throwIoError(
                    target,
                    "Unable to replace destination ZIP archive",
                    std::error_code(static_cast<int>(GetLastError()), std::system_category())
                );
            }
#else
            if (::rename(temporary.c_str(), target.c_str()) != 0) {
                throwIoError(
                    target,
                    "Unable to replace destination ZIP archive",
                    std::error_code(errno, std::generic_category())
                );
            }
#endif
        }

        [[nodiscard]] fs::path publishArchive(
            TemporaryArchive& temporary,
            fs::path          target,
            const fs::path&   destination,
            std::string_view  familyStem,
            ConflictPolicy    conflict
        ) {
            if (conflict == ConflictPolicy::Overwrite) {
                publishWithReplacement(temporary.path(), target);
                temporary.markPublished();
                return target;
            }

            for (;;) {
                try {
                    publishWithoutReplacement(temporary.path(), target);
                    temporary.markPublished();
                    return target;
                } catch (const ProjectError& error) {
                    if (conflict != ConflictPolicy::Rename || error.code() != ProjectErrorCode::DestinationExists) {
                        throw;
                    }
                    target = selectRequestedArchivePath(destination, familyStem, ConflictPolicy::Rename);
                }
            }
        }

    } // namespace

    namespace archive {

        const std::vector<std::string>& defaultFullExcludePatterns() {
            static const std::vector<std::string> patterns{
                "**/.git/**",
                "**/.hg/**",
                "**/.svn/**",
                ".mcdk/**",
                "**/node_modules/**",
                "**/build/**",
                "**/out/**",
                "**/dist/**",
                "**/target/**",
                "**/.cache/**",
                "**/.pytest_cache/**",
                "**/.mypy_cache/**",
                "**/.ruff_cache/**",
                "**/__pycache__/**",
                "**/*.pyc",
                "**/*.pyo",
            };
            return patterns;
        }

        bool isValidExcludePattern(std::string_view pattern) noexcept {
            if (pattern.empty() || pattern.front() == '!' || pattern.front() == '/' || pattern.front() == '\\'
                || pattern.find('\0') != std::string_view::npos) {
                return false;
            }
            if (pattern.size() >= 2 && std::isalpha(static_cast<unsigned char>(pattern[0])) != 0 && pattern[1] == ':') {
                return false;
            }

            std::size_t segmentBegin = 0;
            for (std::size_t index = 0; index <= pattern.size(); ++index) {
                if (index != pattern.size() && pattern[index] != '/' && pattern[index] != '\\') {
                    continue;
                }
                const auto segment = pattern.substr(segmentBegin, index - segmentBegin);
                if (segment == "..") {
                    return false;
                }
                segmentBegin = index + 1;
            }
            return true;
        }

        bool pathMatchesPattern(std::string_view relativePath, std::string_view pattern) {
            if (!isValidExcludePattern(pattern)) {
                return false;
            }
            auto normalizedPath    = normalizeGlobText(relativePath);
            auto normalizedPattern = normalizeGlobText(pattern);
#ifdef _WIN32
            normalizedPath    = asciiLower(std::move(normalizedPath));
            normalizedPattern = asciiLower(std::move(normalizedPattern));
#endif
            return matchGlobSegments(splitGlobSegments(normalizedPath), splitGlobSegments(normalizedPattern));
        }

        std::string sanitizeArchiveStem(std::string_view value) {
            std::string result;
            result.reserve(value.size());
            for (const unsigned char character : value) {
                if (character < 32 || character == '<' || character == '>' || character == ':' || character == '"'
                    || character == '/' || character == '\\' || character == '|' || character == '?'
                    || character == '*') {
                    result.push_back('_');
                } else {
                    result.push_back(static_cast<char>(character));
                }
            }
            while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
                result.pop_back();
            }
            if (result.empty() || result == "." || result == "..") {
                result = "project";
            }

            const auto lower             = asciiLower(result);
            const auto extensionIndex    = lower.find('.');
            const auto reservedCandidate = lower.substr(0, extensionIndex);
            const bool reserved = reservedCandidate == "con" || reservedCandidate == "prn" || reservedCandidate == "aux"
                               || reservedCandidate == "nul"
                               || (reservedCandidate.size() == 4
                                   && (reservedCandidate.starts_with("com") || reservedCandidate.starts_with("lpt"))
                                   && reservedCandidate[3] >= '1' && reservedCandidate[3] <= '9');
            if (reserved) {
                result.insert(result.begin(), '_');
            }
            return result;
        }

        fs::path writeProjectArchive(
            const ProjectSummary&     summary,
            const ExportRequest&      request,
            std::vector<std::string>& warnings
        ) {
            std::error_code error;
            const auto      destinationStatus = fs::status(request.destination, error);
            if (error) {
                throwIoError(request.destination, "Unable to inspect export destination", error);
            }
            if (!fs::is_directory(destinationStatus)) {
                throw ProjectError(
                    ProjectErrorCode::IoError,
                    request.destination,
                    "Export destination must be an existing directory"
                );
            }
            const auto destination = canonicalPath(request.destination);

            auto       familyStem         = projectDirectoryName(summary);
            const auto projectArchiveStem = familyStem;
            if (request.mode == ExportMode::Full) {
                familyStem.append(" ");
                familyStem.append("\xE5\xAE\x8C\xE6\x95\xB4");
            }
            auto target = selectRequestedArchivePath(destination, familyStem, request.conflict);

            std::vector<std::string> patterns;
            std::vector<SourceRoot>  sources;
            CollectionContext        context{
                request.mode == ExportMode::Full
                           ? FilterKind::Full
                           : (summary.kind == ProjectKind::Map ? FilterKind::CleanMap : FilterKind::CleanAddon),
                nullptr,
                destination,
                {},
                projectArchiveStem,
                &warnings,
                {},
                {},
            };

            patterns                = exportExcludePatterns(summary, request.mode);
            context.excludePatterns = &patterns;
            if (request.mode == ExportMode::Full) {
                sources.push_back({canonicalPath(summary.root), {}});
            } else {
                sources = cleanSourceRoots(summary, warnings);
            }

            for (const auto& source : sources) {
                const auto status = fs::symlink_status(source.diskRoot, error);
                if (error) {
                    throwIoError(source.diskRoot, "Unable to inspect archive source root", error);
                }
                if (isLinkOrReparsePoint(source.diskRoot, status) || !fs::is_directory(status)) {
                    throw ProjectError(
                        ProjectErrorCode::InvalidProject,
                        source.diskRoot,
                        "Archive source root must be a real directory"
                    );
                }
                context.excludedDestinationSubtree.reset();
                if (!pathsReferToSameLocation(source.diskRoot, destination)
                    && isPathWithin(destination, source.diskRoot)) {
                    context.excludedDestinationSubtree = destination;
                }
                if (!source.archivePrefix.empty()) {
                    if (shouldExclude(context, source.diskRoot, {}, source.archivePrefix, true)) {
                        continue;
                    }
                    addEntry(context, ArchiveEntry{source.diskRoot, pathToUtf8(source.archivePrefix), 0, true});
                }
                collectDirectory(context, source, source.diskRoot, {});
            }

            std::sort(context.entries.begin(), context.entries.end(), [](const auto& left, const auto& right) {
                if (left.archiveName != right.archiveName) {
                    return left.archiveName < right.archiveName;
                }
                return left.isDirectory && !right.isDirectory;
            });

            TemporaryArchive temporary(temporaryArchivePath(destination));
            {
                ZipWriter writer(temporary.path());
                for (const auto& entry : context.entries) {
                    if (entry.isDirectory) {
                        writer.addDirectory(entry);
                    } else {
                        writer.addFile(entry);
                    }
                }
                writer.finish();
            }
            return publishArchive(temporary, std::move(target), destination, familyStem, request.conflict);
        }

    } // namespace archive

} // namespace mcdk::project
