#include "oxapp/application.h"
#include "oxapp/version.h"

namespace oxapp {

const char* Application::name() noexcept {
    return "oxApp";
}

const char* Application::version() noexcept {
    return OXAPP_VERSION_STRING;
}

std::string Application::platform() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Unknown";
#endif
}

}  // namespace oxapp
