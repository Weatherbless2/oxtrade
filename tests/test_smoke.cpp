#include "oxapp/application.h"

#include <cstring>
#include <iostream>

int main() {
    if (std::strcmp(oxapp::Application::name(), "oxApp") != 0) {
        std::cerr << "unexpected application name\n";
        return 1;
    }

    if (std::strlen(oxapp::Application::version()) == 0) {
        std::cerr << "application version is empty\n";
        return 1;
    }

    if (oxapp::Application::platform().empty()) {
        std::cerr << "platform name is empty\n";
        return 1;
    }

    return 0;
}
