#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <mcdk/project_operations.hpp>

namespace mcdk::project::archive {

    // These helpers are public to mcdk_core so the CLI and focused integration
    // tests exercise exactly the same validation and naming rules as exportProject.
    [[nodiscard]] const std::vector<std::string>& defaultFullExcludePatterns();
    [[nodiscard]] bool                            isValidExcludePattern(std::string_view pattern) noexcept;
    [[nodiscard]] bool        pathMatchesPattern(std::string_view relativePath, std::string_view pattern);
    [[nodiscard]] std::string sanitizeArchiveStem(std::string_view value);

    [[nodiscard]] std::filesystem::path writeProjectArchive(
        const ProjectSummary&     summary,
        const ExportRequest&      request,
        std::vector<std::string>& warnings
    );

} // namespace mcdk::project::archive
