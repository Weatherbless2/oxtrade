#include "oxmbs/lbm/funclist_registry.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace oxmbs::lbm {
namespace {

struct XmlTag {
    std::string name;
    std::map<std::string, std::string> attributes;
    bool closing{false};
    bool self_closing{false};
};

bool is_name_start(char value) noexcept {
    const auto c = static_cast<unsigned char>(value);
    return std::isalpha(c) != 0 || value == '_';
}

bool is_name_character(char value) noexcept {
    const auto c = static_cast<unsigned char>(value);
    return std::isalnum(c) != 0 || value == '_' || value == '-' || value == ':' || value == '.';
}

void skip_whitespace(const std::string& text, std::size_t& cursor) {
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
        ++cursor;
    }
}

bool skip_special(const std::string& text, std::size_t& cursor, std::string& error) {
    if (text.compare(cursor, 4, "<!--") == 0) {
        const auto end = text.find("-->", cursor + 4);
        if (end == std::string::npos) {
            error = "unterminated XML comment";
            return false;
        }
        cursor = end + 3;
        return true;
    }
    if (text.compare(cursor, 2, "<?") == 0) {
        const auto end = text.find("?>", cursor + 2);
        if (end == std::string::npos) {
            error = "unterminated XML declaration or processing instruction";
            return false;
        }
        cursor = end + 2;
        return true;
    }
    return false;
}

bool read_name(const std::string& text, std::size_t& cursor, std::string& name) {
    if (cursor >= text.size() || !is_name_start(text[cursor])) {
        return false;
    }
    const auto begin = cursor++;
    while (cursor < text.size() && is_name_character(text[cursor])) {
        ++cursor;
    }
    name = text.substr(begin, cursor - begin);
    return true;
}

bool read_tag(const std::string& text, std::size_t& cursor, XmlTag& tag, std::string& error) {
    if (cursor >= text.size() || text[cursor] != '<') {
        error = "expected '<' while reading XML tag";
        return false;
    }
    ++cursor;
    if (cursor < text.size() && text[cursor] == '/') {
        tag.closing = true;
        ++cursor;
    }
    if (!read_name(text, cursor, tag.name)) {
        error = "XML tag name is missing or invalid";
        return false;
    }
    if (tag.closing) {
        skip_whitespace(text, cursor);
        if (cursor >= text.size() || text[cursor] != '>') {
            error = "closing XML tag must end with '>'";
            return false;
        }
        ++cursor;
        return true;
    }

    while (cursor < text.size()) {
        skip_whitespace(text, cursor);
        if (cursor >= text.size()) {
            error = "unterminated XML opening tag";
            return false;
        }
        if (text[cursor] == '>') {
            ++cursor;
            return true;
        }
        if (text[cursor] == '/' && cursor + 1 < text.size() && text[cursor + 1] == '>') {
            tag.self_closing = true;
            cursor += 2;
            return true;
        }

        std::string attribute_name;
        if (!read_name(text, cursor, attribute_name)) {
            error = "XML attribute name is invalid in <" + tag.name + ">";
            return false;
        }
        skip_whitespace(text, cursor);
        if (cursor >= text.size() || text[cursor] != '=') {
            error = "XML attribute '" + attribute_name + "' is missing '='";
            return false;
        }
        ++cursor;
        skip_whitespace(text, cursor);
        if (cursor >= text.size() || (text[cursor] != '\"' && text[cursor] != '\'')) {
            error = "XML attribute '" + attribute_name + "' must use quoted text";
            return false;
        }
        const char quote = text[cursor++];
        const auto value_begin = cursor;
        while (cursor < text.size() && text[cursor] != quote) {
            if (text[cursor] == '<') {
                error = "XML attribute '" + attribute_name + "' contains an unescaped '<'";
                return false;
            }
            ++cursor;
        }
        if (cursor >= text.size()) {
            error = "XML attribute '" + attribute_name + "' quote is not closed";
            return false;
        }
        const auto value = text.substr(value_begin, cursor - value_begin);
        ++cursor;
        if (!tag.attributes.emplace(attribute_name, value).second) {
            error = "XML tag <" + tag.name + "> contains a duplicate attribute";
            return false;
        }
    }

    error = "unterminated XML opening tag";
    return false;
}

bool parse_uint32(const std::string& raw, std::uint32_t& value) {
    if (raw.empty()) {
        return false;
    }
    std::uint64_t parsed{0};
    const auto parsed_result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (parsed_result.ec != std::errc{} || parsed_result.ptr != raw.data() + raw.size() ||
        parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool build_route(const XmlTag& tag,
                 const std::filesystem::path& directory,
                 FunctionRoute& route,
                 std::string& error) {
    if (!tag.self_closing) {
        error = "<function> must be self-closing";
        return false;
    }
    if (tag.attributes.size() != 4 || tag.attributes.count("msg_type") == 0 ||
        tag.attributes.count("func_id") == 0 || tag.attributes.count("library") == 0 ||
        tag.attributes.count("symbol") == 0) {
        error = "<function> must contain exactly msg_type, func_id, library and symbol attributes";
        return false;
    }
    if (!parse_uint32(tag.attributes.at("msg_type"), route.key.msg_type) ||
        !parse_uint32(tag.attributes.at("func_id"), route.key.func_id)) {
        error = "<function> msg_type and func_id must be non-zero uint32 decimal values";
        return false;
    }
    if (tag.attributes.at("library").empty() || tag.attributes.at("symbol").empty()) {
        error = "<function> library and symbol must not be empty";
        return false;
    }

    const std::filesystem::path configured_library{tag.attributes.at("library")};
    route.library_path = configured_library.is_absolute()
                             ? configured_library.lexically_normal()
                             : (directory / configured_library).lexically_normal();  // 相对路径固定以 XML 所在目录为基准。
    route.symbol = tag.attributes.at("symbol");
    return true;
}

}  // namespace

bool FunctionRouteKey::operator<(const FunctionRouteKey& other) const noexcept {
    return msg_type != other.msg_type ? msg_type < other.msg_type : func_id < other.func_id;
}

const char* funclist_status_name(FunclistStatus status) noexcept {
    switch (status) {
    case FunclistStatus::Ok:
        return "Ok";
    case FunclistStatus::FileOpenFailed:
        return "FileOpenFailed";
    case FunclistStatus::InvalidXml:
        return "InvalidXml";
    case FunclistStatus::InvalidRoot:
        return "InvalidRoot";
    case FunclistStatus::InvalidFunction:
        return "InvalidFunction";
    case FunclistStatus::DuplicateRoute:
        return "DuplicateRoute";
    }
    return "Unknown";
}

FunclistLoadResult FunclistRegistry::load_from_file(const std::filesystem::path& path) {
    FunclistLoadResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "cannot open funclist file '" + path.string() + "'";
        return result;
    }
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (stream.bad()) {
        result.error = "cannot read funclist file '" + path.string() + "'";
        return result;
    }

    std::map<FunctionRouteKey, FunctionRoute> candidate_routes;
    const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
    const auto directory = absolute_path.parent_path();
    std::size_t cursor{0};
    bool root_open{false};
    bool root_closed{false};
    bool saw_root{false};

    while (cursor < text.size()) {
        skip_whitespace(text, cursor);
        if (cursor >= text.size()) {
            break;
        }
        std::string parse_error;
        if (skip_special(text, cursor, parse_error)) {
            continue;
        }
        if (!parse_error.empty()) {
            result.status = FunclistStatus::InvalidXml;
            result.error = parse_error;
            return result;
        }
        if (text[cursor] != '<') {
            result.status = FunclistStatus::InvalidXml;
            result.error = "non-whitespace text is not allowed in funclist.xml";
            return result;
        }

        XmlTag tag;
        if (!read_tag(text, cursor, tag, parse_error)) {
            result.status = FunclistStatus::InvalidXml;
            result.error = parse_error;
            return result;
        }
        if (!saw_root) {
            saw_root = true;
            if (tag.closing || tag.self_closing || tag.name != "funclist" || !tag.attributes.empty()) {
                result.status = FunclistStatus::InvalidRoot;
                result.error = "root element must be exactly <funclist>";
                return result;
            }
            root_open = true;
            continue;
        }
        if (root_closed) {
            result.status = FunclistStatus::InvalidXml;
            result.error = "content appears after </funclist>";
            return result;
        }
        if (tag.closing) {
            if (!root_open || tag.name != "funclist") {
                result.status = FunclistStatus::InvalidXml;
                result.error = "unexpected closing tag </" + tag.name + ">";
                return result;
            }
            root_open = false;
            root_closed = true;
            continue;
        }
        if (!root_open || tag.name != "function") {
            result.status = FunclistStatus::InvalidFunction;
            result.error = "only self-closing <function/> entries are allowed under <funclist>";
            return result;
        }

        FunctionRoute route;
        if (!build_route(tag, directory, route, parse_error)) {
            result.status = FunclistStatus::InvalidFunction;
            result.error = parse_error;
            return result;
        }
        if (!candidate_routes.emplace(route.key, std::move(route)).second) {
            result.status = FunclistStatus::DuplicateRoute;
            result.error = "duplicate <function> route for msg_type/func_id";
            return result;
        }
    }

    if (!saw_root || root_open || !root_closed) {
        result.status = FunclistStatus::InvalidRoot;
        result.error = "funclist.xml must contain a complete <funclist>...</funclist> root";
        return result;
    }

    routes_ = std::move(candidate_routes);  // 所有校验完成后原子替换，错误 XML 不会污染上一份可用配置。
    source_path_ = absolute_path;
    result.status = FunclistStatus::Ok;
    return result;
}

const FunctionRoute* FunclistRegistry::find(std::uint32_t msg_type, std::uint32_t func_id) const noexcept {
    const auto found = routes_.find(FunctionRouteKey{msg_type, func_id});
    return found == routes_.end() ? nullptr : &found->second;
}

std::size_t FunclistRegistry::size() const noexcept {
    return routes_.size();
}

const std::filesystem::path& FunclistRegistry::source_path() const noexcept {
    return source_path_;
}

}  // namespace oxmbs::lbm
