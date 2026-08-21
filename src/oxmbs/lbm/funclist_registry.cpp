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
    if (raw.empty()) {  // 空编号没有业务含义，先拒绝，避免空字符串被误当成 0。
        return false;  // 调用方会把它转换成 InvalidFunction，不会继续生成半条路由。
    }
    std::uint64_t parsed{0};  // 先用更宽的整数承接文本，随后才能安全检查 uint32 边界。
    const auto parsed_result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);  // 只接受整段十进制文本，不接受前缀或尾随垃圾。
    if (parsed_result.ec != std::errc{} || parsed_result.ptr != raw.data() + raw.size() ||
        parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {  // 0、溢出和未完整解析都会让路由键失效。
        return false;  // 错误在配置加载阶段暴露，Worker 不会收到不可定位的任务。
    }
    value = static_cast<std::uint32_t>(parsed);  // 范围确认后再缩窄，避免高位截断改变业务编号。
    return true;  // 调用方可以继续检查库路径和导出符号。
}

bool build_route(const XmlTag& tag,
                 const std::filesystem::path& directory,
                 FunctionRoute& route,
                 std::string& error) {
    if (!tag.self_closing) {  // 路由节点不允许包裹文本或未知子节点，格式必须固定可审计。
        error = "<function> must be self-closing";  // 错误文本会回到启动日志，帮助定位 XML。
        return false;  // 不把不完整的 route 放入候选表。
    }
    if (tag.attributes.size() != 4 || tag.attributes.count("msg_type") == 0 ||
        tag.attributes.count("func_id") == 0 || tag.attributes.count("library") == 0 ||
        tag.attributes.count("symbol") == 0) {  // 白名单限制属性集合，拼写错误不会静默变成默认值。
        error = "<function> must contain exactly msg_type, func_id, library and symbol attributes";  // 明确告诉配置维护者缺了什么契约。
        return false;  // 当前条目失败即终止本次候选配置构建。
    }
    if (!parse_uint32(tag.attributes.at("msg_type"), route.key.msg_type) ||
        !parse_uint32(tag.attributes.at("func_id"), route.key.func_id)) {  // 两个编号共同决定最终调用哪个业务函数。
        error = "<function> msg_type and func_id must be non-zero uint32 decimal values";  // 失败时不会出现模糊的路由未找到。
        return false;  // 不允许 0 或超范围编号进入 map。
    }
    if (tag.attributes.at("library").empty() || tag.attributes.at("symbol").empty()) {  // 空路径或空符号无法被 dlopen/dlsym 使用。
        error = "<function> library and symbol must not be empty";  // 启动阶段报告配置责任，而不是等任务到来才失败。
        return false;  // 路由对象保持未构造状态。
    }

    const std::filesystem::path configured_library{tag.attributes.at("library")};  // XML 文本先转成路径对象，后续统一规范化。
    route.library_path = configured_library.is_absolute()
                             ? configured_library.lexically_normal()
                             : (directory / configured_library).lexically_normal();  // 相对路径固定以 XML 所在目录为基准。
    route.symbol = tag.attributes.at("symbol");  // 保存 extern "C" 名称，真正调用前才执行 dlsym。
    return true;  // 这里只代表配置结构合法，不代表动态库已经存在。
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
    std::ifstream stream(path, std::ios::binary);  // 启动期读取完整 XML；二进制模式避免换行转换影响游标和诊断。
    if (!stream) {
        result.error = "cannot open funclist file '" + path.string() + "'";
        return result;
    }
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};  // 保留完整原文，解析失败可在本次调用内安全回滚。
    if (stream.bad()) {
        result.error = "cannot read funclist file '" + path.string() + "'";
        return result;
    }

    std::map<FunctionRouteKey, FunctionRoute> candidate_routes;  // 先构建临时表，避免边读边污染当前生效路由。
    const auto absolute_path = std::filesystem::absolute(path).lexically_normal();  // 固定源文件身份，日志和库路径不依赖 cwd。
    const auto directory = absolute_path.parent_path();  // 所有相对 library 都从 XML 所在目录计算。
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
        if (skip_special(text, cursor, parse_error)) {  // 跳过 XML 声明和注释，不把它们误判成业务节点。
            continue;  // cursor 已由 helper 推进到特殊片段之后。
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
        if (!read_tag(text, cursor, tag, parse_error)) {  // 读取失败立即停止，防止使用不完整的属性集合。
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
            root_open = true;  // 进入根节点范围，后续只能接受 function 或根闭合标签。
            continue;  // 根标签本身没有路由数据。
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
        if (!build_route(tag, directory, route, parse_error)) {  // 当前 function 的四字段和路径在这里一次性校验。
            result.status = FunclistStatus::InvalidFunction;
            result.error = parse_error;
            return result;
        }
        if (!candidate_routes.emplace(route.key, std::move(route)).second) {  // 相同二元键重复时拒绝后者，不能静默覆盖前者。
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

    routes_ = std::move(candidate_routes);  // 全部通过后一次替换；任何中途错误都保留上一份可用路由。
    source_path_ = absolute_path;  // 记录实际加载文件，后续诊断可复现路径来源。
    result.status = FunclistStatus::Ok;  // 只有此状态才允许 Dispatcher 继续启动 Worker。
    return result;  // 成功不表示 .so 已加载，动态库仍由任务首次命中时懒加载。
}

const FunctionRoute* FunclistRegistry::find(std::uint32_t msg_type, std::uint32_t func_id) const noexcept {
    const auto found = routes_.find(FunctionRouteKey{msg_type, func_id});  // 用业务二元键精确查找，不扫描也不猜测默认函数。
    return found == routes_.end() ? nullptr : &found->second;  // nullptr 会被 Loader 转成 RouteNotFound/Rejected。
}

std::size_t FunclistRegistry::size() const noexcept {
    return routes_.size();
}

const std::filesystem::path& FunclistRegistry::source_path() const noexcept {
    return source_path_;
}

}  // namespace oxmbs::lbm
