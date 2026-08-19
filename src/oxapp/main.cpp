#include "oxapp/application.h"

#include <iostream>

int main() {
    std::cout << oxapp::Application::name() << " started\n";
    std::cout << "version: " << oxapp::Application::version() << '\n';
    std::cout << "platform: " << oxapp::Application::platform() << '\n';
    return 0;
}
