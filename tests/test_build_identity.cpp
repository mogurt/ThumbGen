#include <thumbgen/core.h>
#include "build_identity.h"
#include <iostream>

int main() {
    try {
        const auto capabilities = thumbgen::Engine::capabilities();
        const nlohmann::json expected{
            {"build", capabilities.at("build")},
            {"version", capabilities.at("version")},
            {"oiio_version", capabilities.at("oiio_version")}
        };
        const auto& first = thumbgen::detail::immutable_build_identity();
        const auto& second = thumbgen::detail::immutable_build_identity();
        if (first != expected || &first != &second || first.size() != 3) {
            std::cerr << "immutable handshake identity differs from public capabilities\n";
            return 1;
        }
        if (!first.at("build").at("dirty").is_boolean()) {
            std::cerr << "identity dirty flag must retain boolean wire type\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
