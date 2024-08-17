#include <string>
#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

namespace tl = thallium;

void hello(const tl::request& req, const std::string& name) {
    (void)req;
    std::cout << "Hello " << name << std::endl;
}

int main() {

    tl::engine myEngine("tcp", THALLIUM_SERVER_MODE);
    std::cout << "Server running at address " << myEngine.self() << std::endl;
    myEngine.define("hello", hello).disable_response();

    myEngine.wait_for_finalize();

    return 0;
}

