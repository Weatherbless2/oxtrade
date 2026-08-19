#pragma once

#include <string>

namespace oxapp {

class Application {
public:
    static const char* name() noexcept;
    static const char* version() noexcept;
    static std::string platform();
};

}  // namespace oxapp
