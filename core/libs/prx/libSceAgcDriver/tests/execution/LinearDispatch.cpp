#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t Groups = 73728;
constexpr std::uint32_t Untouched = 0xcafebabeu;
alignas(256) std::array<std::uint32_t, Groups * 4 + 4> Output{};
alignas(256) constexpr std::array<std::uint32_t, 8> Code{
    0x7e000204u, 0x7e020204u, 0x7e040205u, 0x7e060206u, 0x7e080281u,
    0xe0782000u, 0x80000100u, 0xbf810000u};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::array<std::uint32_t, 4> Descriptor() {
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    return {
        static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32) & 0xffffu) | (16u << 16), Groups, 0x0004dfacu};
}

void Verify(std::uint32_t groups) {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
    for (std::uint32_t i = 0; i < Groups + 1; ++i) {
        const std::array<std::uint32_t, 4> expected = i < groups ? std::array<std::uint32_t, 4>{i, 0, 0, 1} : std::array<std::uint32_t, 4>{Untouched, Untouched, Untouched, Untouched};
        Require(std::equal(expected.begin(), expected.end(), Output.begin() + i * 4), "linear dispatch output mismatch at group " + std::to_string(i) + ": " + std::to_string(Output[i * 4]) + "," + std::to_string(Output[i * 4 + 1]) + "," + std::to_string(Output[i * 4 + 2]) + "," + std::to_string(Output[i * 4 + 3]));
    }
}

void Run(std::uint32_t groups) {
    Output.fill(Untouched);
    const auto address = reinterpret_cast<std::uintptr_t>(Code.data());
    const auto descriptor = Descriptor();
    std::vector<std::uint32_t> words;
    const auto set = [&](std::uint32_t offset, std::uint32_t value) {
        words.insert(words.end(), {0xc0017600u, offset, value});
    };
    set(0x20c, static_cast<std::uint32_t>(address >> 8));
    set(0x20d, static_cast<std::uint32_t>((address >> 40) & 0xffu));
    for (const auto offset : {0x207u, 0x208u, 0x209u}) set(offset, 1);
    set(0x212, 0);
    set(0x213, (4u << 1) | (7u << 7));
    for (std::uint32_t i = 0; i < descriptor.size(); ++i) set(0x240 + i, descriptor[i]);
    words.insert(words.end(), {0xc0031500u, groups, 1, 1, 0x8041u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    Verify(groups);
}

void Folded(AgcDriver::VulkanDevice& device, std::uint32_t wave, bool partial) {
    Output.fill(Untouched);
    const auto descriptor = Descriptor();
    const auto address = reinterpret_cast<std::uintptr_t>(Code.data());
    const std::array memory{ShaderRecompiler::MemoryRegion{address, std::as_bytes(std::span(Code))}};
    ShaderRecompiler::ShaderComputeStageInfo compute{{2, 1, 1}, 0, {true, true, true}, false, 1};
    compute.linearWorkgroups = true;
    if (partial) compute.partialThreads = {1022, 1, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, address, Code, 0, {}},
        {wave, 0, descriptor, compute, std::nullopt, std::nullopt, memory},
        device.ComputeTarget(wave), {0, 0, 0, 128}};
    const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(request);
    const auto replay = ShaderRecompiler::RequestSerializer{}.Deserialize(text);
    Require(replay.request.context.compute->linearWorkgroups, "serialized request lost linear workgroups");
    const auto compiled = ShaderRecompiler::Recompile(replay.request);
    auto ordinary = request;
    ordinary.context.compute->linearWorkgroups = false;
    Require(ShaderRecompiler::Recompile(ordinary).PipelineVariantId() != compiled.PipelineVariantId(), "folded and ordinary dispatches shared a compiled variant");
    device.Dispatch(compiled, 256, 2, 1);
    device.WaitIdle();
    Verify(partial ? 511 : 512);
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const auto wave : {32u, 64u}) {
            Folded(*device, wave, false);
            Folded(*device, wave, true);
        }
        device.reset();
        Run(128);
        Run(Groups);
        Run(128);
        Run(Groups);
        AgcDriverShutdown_nid_postfix();
        std::cout << "linear dispatch workgroup IDs and output bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
