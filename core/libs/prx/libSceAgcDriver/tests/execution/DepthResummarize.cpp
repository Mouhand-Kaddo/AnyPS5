#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::DrawOptions;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::HtileBytes;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Surface;
using DepthFastClearHarness::Width;
constexpr std::size_t Tiles = HtileBytes / 4u;
alignas(4096) std::array<float, Width * Height * 2> Depth{};
alignas(256) std::array<std::uint32_t, 1024> Htile{};

template<typename Action>
void Rejected(Action action) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find("resummariz") != std::string_view::npos, "unexpected depth resummarization rejection");
        return;
    }
    throw std::runtime_error("unsupported depth resummarization did not throw");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = Compile(*device);
        const Surface surface{reinterpret_cast<std::uintptr_t>(Depth.data()), 0, reinterpret_cast<std::uintptr_t>(Htile.data()), false, VK_FORMAT_D32_SFLOAT};
        const DrawOptions resummarize{.depthCompare = VK_COMPARE_OP_ALWAYS, .resummarize = true};
        Htile.fill(0xfffc000fu);
        Rejected([&] { Draw(*device, shaders, surface, resummarize); });
        Require(Draw(*device, shaders, surface, {.depthWrite = true}) == Covered, "initial depth write did not cover the surface");
        Require(Draw(*device, shaders, surface, {}) == 0, "depth write did not survive an ordinary bind");
        Htile.fill(0u);
        const auto cleared = Htile;
        Rejected([&] { Draw(*device, shaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .resummarize = true}); });
        Require(Htile == cleared, "rejected writing resummarization changed the HTILE");
        Require(Draw(*device, shaders, surface, resummarize) == 0, "resummarization wrote color pixels");
        Require(std::all_of(Htile.begin(), Htile.begin() + Tiles, [](auto word) { return word == 0xfffc000fu; }), "resummarized HTILE did not become expanded");
        Require(Draw(*device, shaders, surface, {.depthWrite = true}) == Covered, "resummarization did not materialize the uniform fast clear");
        Rejected([&] { Draw(*device, shaders, surface, resummarize); });
        Require(Draw(*device, shaders, surface, {}) == 0, "expanded resummarization cleared the existing depth");
        Htile.fill(0u);
        Htile[Tiles - 1] = 0xfffc000fu;
        const auto mixed = Htile;
        Rejected([&] { Draw(*device, shaders, surface, resummarize); });
        Require(Htile == mixed, "rejected mixed HTILE was modified");
        Htile.fill(1u);
        Rejected([&] { Draw(*device, shaders, surface, resummarize); });
        Require(std::all_of(Htile.begin(), Htile.end(), [](auto word) { return word == 1u; }), "rejected compressed HTILE was modified");
        Htile.fill(0u);
        const Surface d16{reinterpret_cast<std::uintptr_t>(Depth.data() + Width * Height), 0, surface.htile, false, VK_FORMAT_D16_UNORM};
        Require(Draw(*device, shaders, d16, resummarize) == 0, "D16 resummarization wrote color pixels");
        Require(Draw(*device, shaders, d16, {}) == Covered, "D16 resummarization did not retain the clear value");
        std::cout << "read-only depth resummarization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
