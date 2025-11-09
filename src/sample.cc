#include "sample.h"

#include <iostream>

#include "uv.h"

void uv_init() {
    unsigned int uv_major = UV_VERSION_MAJOR;
    unsigned int uv_minor = UV_VERSION_MINOR;

    std::cout << "libuv successfully linked and built!" << std::endl;
    std::cout << "Detected libuv version: " << uv_major << "." << uv_minor << std::endl;
}
