#pragma once

#include <optional>
#include <string_view>

struct EmbeddedAsset {
    std::string_view content;
    std::string_view mime_type;
};

std::optional<EmbeddedAsset> find_embedded_asset(std::string_view uri);
