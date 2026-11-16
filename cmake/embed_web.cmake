if(NOT DEFINED WEB_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "WEB_DIR and OUTPUT_FILE must be defined")
endif()

get_filename_component(OUTPUT_DIR "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

file(GLOB_RECURSE WEB_REL_FILES RELATIVE "${WEB_DIR}" "${WEB_DIR}/*")
list(SORT WEB_REL_FILES)

function(get_mime_type path out_var)
    get_filename_component(ext "${path}" LAST_EXT)
    string(TOLOWER "${ext}" ext)
    if(ext STREQUAL ".html" OR ext STREQUAL ".htm")
        set(${out_var} "text/html; charset=utf-8" PARENT_SCOPE)
    elseif(ext STREQUAL ".js" OR ext STREQUAL ".mjs")
        set(${out_var} "application/javascript; charset=utf-8" PARENT_SCOPE)
    elseif(ext STREQUAL ".css")
        set(${out_var} "text/css; charset=utf-8" PARENT_SCOPE)
    elseif(ext STREQUAL ".json")
        set(${out_var} "application/json; charset=utf-8" PARENT_SCOPE)
    elseif(ext STREQUAL ".svg")
        set(${out_var} "image/svg+xml" PARENT_SCOPE)
    elseif(ext STREQUAL ".png")
        set(${out_var} "image/png" PARENT_SCOPE)
    elseif(ext STREQUAL ".jpg" OR ext STREQUAL ".jpeg")
        set(${out_var} "image/jpeg" PARENT_SCOPE)
    elseif(ext STREQUAL ".webp")
        set(${out_var} "image/webp" PARENT_SCOPE)
    elseif(ext STREQUAL ".ico")
        set(${out_var} "image/x-icon" PARENT_SCOPE)
    elseif(ext STREQUAL ".wasm")
        set(${out_var} "application/wasm" PARENT_SCOPE)
    elseif(ext STREQUAL ".txt")
        set(${out_var} "text/plain; charset=utf-8" PARENT_SCOPE)
    else()
        set(${out_var} "application/octet-stream" PARENT_SCOPE)
    endif()
endfunction()

set(ARRAYS_CODE "")
set(MAP_ENTRIES "")
set(IDX 0)

foreach(rel_path IN LISTS WEB_REL_FILES)
    set(abs_path "${WEB_DIR}/${rel_path}")
    if(IS_DIRECTORY "${abs_path}")
        continue()
    endif()

    # Normalize Windows backslashes if any
    string(REPLACE "\\" "/" norm_path "${rel_path}")

    get_mime_type("${norm_path}" mime_type)

    file(READ "${abs_path}" hex_str HEX)
    string(LENGTH "${hex_str}" hex_len)
    math(EXPR byte_len "${hex_len} / 2")

    if(byte_len GREATER 0)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," c_bytes "${hex_str}")
        string(APPEND ARRAYS_CODE "const unsigned char asset_${IDX}[] = { ${c_bytes} };\n")
        string(APPEND MAP_ENTRIES "        {\"${norm_path}\", {std::string_view(reinterpret_cast<const char*>(asset_${IDX}), ${byte_len}), \"${mime_type}\"}},\n")
    else()
        string(APPEND MAP_ENTRIES "        {\"${norm_path}\", {std::string_view{}, \"${mime_type}\"}},\n")
    endif()

    math(EXPR IDX "${IDX} + 1")
endforeach()

set(GENERATED_CONTENT "#include \"web/embedded_assets.hpp\"
#include <unordered_map>

namespace {
${ARRAYS_CODE}} // namespace

std::optional<EmbeddedAsset> find_embedded_asset(std::string_view uri) {
    auto q = uri.find_first_of(\"?#\");
    if (q != std::string_view::npos) {
        uri = uri.substr(0, q);
    }
    while (!uri.empty() && uri.front() == '/') {
        uri.remove_prefix(1);
    }
    if (uri.empty()) {
        uri = \"index.html\";
    }

    static const std::unordered_map<std::string_view, EmbeddedAsset> assets = {
${MAP_ENTRIES}    };

    auto it = assets.find(uri);
    if (it == assets.end()) {
        return std::nullopt;
    }
    return it->second;
}
")

file(WRITE "${OUTPUT_FILE}" "${GENERATED_CONTENT}")
