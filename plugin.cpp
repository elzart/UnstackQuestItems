#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "UnstackQuestItems.h"

using namespace std::literals;

// ============================================
// Plugin Declaration
// ============================================
SKSEPluginInfo(
    .Version = { 1, 0, 0, 0 },
    .Name = "UnstackQuestItems"sv,
    .Author = "Author"sv,
    .SupportEmail = ""sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary
)

// ============================================
// Setup Logging
// ============================================
void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

// ============================================
// Plugin Entry Point
// ============================================
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SetupLog();

    auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    SKSE::log::info("{} v{}", plugin->GetName(), plugin->GetVersion());
    SKSE::log::info("Game version: {}", skse->RuntimeVersion().string());

    SKSE::Init(skse);

    SKSE::AllocTrampoline(256);

    UnstackQuestItems::AddToItemListHook::Install();

    SKSE::log::info("{} loaded", plugin->GetName());

    return true;
}
