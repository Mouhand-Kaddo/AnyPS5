#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "execution/VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>

using Entry = void (APS5_VABI*)(std::uint64_t, std::uint64_t);

extern "C" {
std::int32_t APS5_VABI _sceFiberInitializeImpl_nid_postfix(FiberObject*, const char*, Entry, std::uint64_t, void*, std::uint64_t, const void*, std::uint32_t);
std::int32_t APS5_VABI sceFiberRun_nid_postfix(FiberObject*, std::uint64_t, std::uint64_t*);
std::int32_t APS5_VABI sceFiberReturnToThread(std::uint64_t, std::uint64_t*);
std::int32_t APS5_VABI sceFiberFinalize(FiberObject*);
}

namespace {

constexpr std::size_t PaddingBytes = 256 * 1024;
constexpr std::size_t ContextBytes = 32 * 1024;
constexpr unsigned char Sentinel = 0xa5;

struct alignas(16) Backing {
    std::array<unsigned char, PaddingBytes + ContextBytes> bytes;
};

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Shader preparation stack: %s\n", message);
        std::abort();
    }
}

void APS5_VABI Prepare(std::uint64_t action, std::uint64_t argument) {
    if (action == 3) {
        const auto* registered = reinterpret_cast<const Shader*>(argument);
        AgcDriverRegisterShader_nid_postfix(registered);
        const std::array<ShaderRegister, 1> context{{{0x1b6, 1}}};
        AgcDriverResolveShaderAbi_nid_postfix(registered, context, {});
        Require(sceFiberReturnToThread(action + 1, nullptr) == 0, "return to thread");
        Require(false, "finished fiber resumed");
    }
    Shader shader{};
    const std::array<std::string_view, 3> errors{
        "invalid shader header",
        "static ABI refers to an unregistered shader",
        "graphics ABI has no shader headers",
    };
    bool rejected = false;
    try {
        switch (action) {
        case 0: AgcDriverRegisterShader_nid_postfix(&shader); break;
        case 1: AgcDriverResolveShaderAbi_nid_postfix(&shader, {}, {}); break;
        case 2: AgcDriverResolveGraphicsStagesAbi_nid_postfix({}, {}, {}); break;
        default: Require(false, "invalid action");
        }
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find(errors[action]) != std::string_view::npos;
    }
    Require(rejected, "preparation did not retain its input validation");
    Require(sceFiberReturnToThread(action + 1, nullptr) == 0, "return to thread");
    Require(false, "finished fiber resumed");
}

void RunFiber(std::uint64_t action, std::uint64_t argument = 0) {
    auto backing = std::make_unique<Backing>();
    backing->bytes.fill(Sentinel);
    alignas(8) std::array<unsigned char, 0x100> storage{};
    auto* fiber = reinterpret_cast<FiberObject*>(storage.data());
    Require(_sceFiberInitializeImpl_nid_postfix(fiber, "Shader preparation", Prepare, action,
        backing->bytes.data() + PaddingBytes, ContextBytes, nullptr, 0) == 0, "initialize fiber");
    std::uint64_t returned = 0;
    Require(sceFiberRun_nid_postfix(fiber, argument, &returned) == 0 && returned == action + 1, "run fiber");
    Require(std::all_of(backing->bytes.begin(), backing->bytes.begin() + PaddingBytes,
        [](unsigned char value) { return value == Sentinel; }), "preparation wrote below its fiber context");
    Require(sceFiberFinalize(fiber) == 0, "finalize fiber");
}

struct ComputeShader {
    alignas(256) std::array<std::uint32_t, 1> code{0xbf810000u};
    struct Header {
        Shader shader{};
        std::array<ShaderRegister, 7> registers{};
        ShaderSpecialRegs specials{};
    } header;

    ComputeShader() {
        const auto address = reinterpret_cast<std::uintptr_t>(code.data());
        header.shader.file_header = 0x34333231u;
        header.shader.version = 0x18;
        header.shader.header_size = sizeof(header);
        header.shader.shader_size = sizeof(code);
        header.shader.code = code.data();
        header.shader.sh_registers = header.registers.data();
        header.shader.num_sh_registers = header.registers.size();
        header.shader.specials = &header.specials;
        header.specials.dispatch_modifier = 0x8000;
        header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x207, 1}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 0}}};
    }
};

}

int main(int argc, char** argv) {
    if (argc == 1) {
        for (std::uint64_t action = 0; action < 3; ++action) RunFiber(action);
        return 0;
    }
    Require(argc == 2 && std::string_view(argv[1]) == "--registered", "invalid test arguments");
    auto device = OpenVulkanTestDevice();
    if (!device) return VulkanTestSkipped;
    device.reset();
    auto shader = std::make_unique<ComputeShader>();
    AgcDriverRegisterShader_nid_postfix(&shader->header.shader);
    shader->header.registers[2].value = 2;
    RunFiber(3, reinterpret_cast<std::uintptr_t>(&shader->header.shader));
    AgcDriverShutdown_nid_postfix();
}
