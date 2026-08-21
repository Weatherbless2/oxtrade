#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace oxmbs::lbm {

struct FunctionRouteKey {
    std::uint32_t msg_type{0};  // 真实 PackMessage 枚举确认后由配置注入，不固化当前 Mock 数字。
    std::uint32_t func_id{0};  // 与 msg_type 组合定位一个导出函数。
    bool operator<(const FunctionRouteKey& other) const noexcept;
};

struct FunctionRoute {
    FunctionRouteKey key;
    std::filesystem::path library_path;  // 相对路径已按 funclist.xml 目录解析，不依赖服务启动 cwd。
    std::string symbol;  // extern "C" 导出的函数名，供 dlsym 查找。
};

enum class FunclistStatus {
    Ok,
    FileOpenFailed,
    InvalidXml,
    InvalidRoot,
    InvalidFunction,
    DuplicateRoute,
};

const char* funclist_status_name(FunclistStatus status) noexcept;

struct FunclistLoadResult {
    FunclistStatus status{FunclistStatus::FileOpenFailed};
    std::string error;
    explicit operator bool() const noexcept { return status == FunclistStatus::Ok; }
};

class FunclistRegistry {
public:
    FunclistLoadResult load_from_file(const std::filesystem::path& path);  // 完整校验后才替换活动映射。
    const FunctionRoute* find(std::uint32_t msg_type, std::uint32_t func_id) const noexcept;
    std::size_t size() const noexcept;
    const std::filesystem::path& source_path() const noexcept;

private:
    std::map<FunctionRouteKey, FunctionRoute> routes_;  // 有序映射让诊断顺序稳定，加载只发生在启动期。
    std::filesystem::path source_path_;
};

}  // namespace oxmbs::lbm
