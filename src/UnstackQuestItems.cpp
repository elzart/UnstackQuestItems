#include "UnstackQuestItems.h"
#include <SimpleIni.h>
#include <atomic>
#include <vector>
#include <filesystem>

namespace UnstackQuestItems {

    // ============================================================
    // CONFIG
    // ============================================================

    struct Config {
        bool debugLogging = false;

        static Config& Get() {
            static Config instance;
            return instance;
        }

        void Load() {
            const auto dataPath = std::filesystem::current_path() / "Data";
            const auto iniPath = dataPath / "SKSE" / "Plugins" / "UnstackQuestItems.ini";

            if (!std::filesystem::exists(iniPath)) {
                return;
            }

            CSimpleIniA ini;
            ini.SetUnicode();

            if (ini.LoadFile(iniPath.string().c_str()) < 0) {
                return;
            }

            debugLogging = ini.GetBoolValue("General", "bDebugLogging", false);
        }
    };

    // ============================================================
    // GLOBALS
    // ============================================================

    static std::atomic<uint64_t> g_addToItemListCalls{0};
    static std::atomic<uint64_t> g_splitCalls{0};

    // AddToItemList: internal game function that adds an InventoryEntryData to
    // the UI item list when the inventory menu is built.
    //   void* AddToItemList(void* itemList, RE::InventoryEntryData* entry, void* param3)
    using AddToItemList_t = void*(*)(void*, RE::InventoryEntryData*, void*);
    static AddToItemList_t g_originalAddToItemList = nullptr;

    // ============================================================
    // QUEST-FLAG DETECTION
    // ============================================================

    static bool IsQuestExtraDataList(RE::ExtraDataList* xList) {
        if (!xList) {
            return false;
        }

        if (!xList->HasType(RE::ExtraDataType::kAliasInstanceArray)) {
            return false;
        }

        auto* aliasArray = xList->GetByType<RE::ExtraAliasInstanceArray>();
        if (!aliasArray) {
            return false;
        }

        for (auto* instanceData : aliasArray->aliases) {
            if (instanceData && instanceData->alias &&
                instanceData->alias->IsQuestObject()) {
                return true;
            }
        }

        return false;
    }

    // ============================================================
    // MENU HANDLER (diagnostics when debug logging is on)
    // ============================================================

    class MenuEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuEventHandler* GetSingleton() {
            static MenuEventHandler singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*
        ) override {
            if (Config::Get().debugLogging && a_event && a_event->opening &&
                a_event->menuName == RE::InventoryMenu::MENU_NAME) {
                SKSE::log::info("=== INVENTORY OPENED ===");
                SKSE::log::info("  AddToItemList calls: {}", g_addToItemListCalls.load());
                SKSE::log::info("  Split operations: {}", g_splitCalls.load());
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ============================================================
    // HOOKED ADDTOITEMLIST
    // ============================================================

    static void* HookedAddToItemList(
        void* a_itemList,
        RE::InventoryEntryData* a_entry,
        void* a_param3
    ) {
        g_addToItemListCalls++;

        if (!a_entry || !a_itemList || !g_originalAddToItemList) {
            if (g_originalAddToItemList) {
                return g_originalAddToItemList(a_itemList, a_entry, a_param3);
            }
            return nullptr;
        }

        std::int32_t questCount = 0;
        std::vector<RE::ExtraDataList*> questLists;
        std::vector<RE::ExtraDataList*> normalLists;

        if (a_entry->extraLists) {
            for (auto* xList : *a_entry->extraLists) {
                if (xList) {
                    if (IsQuestExtraDataList(xList)) {
                        auto* countExtra = xList->GetByType<RE::ExtraCount>();
                        questCount += countExtra ? countExtra->count : 1;
                        questLists.push_back(xList);
                    } else {
                        normalLists.push_back(xList);
                    }
                }
            }
        }

        std::int32_t normalCount = a_entry->countDelta - questCount;

        if (questCount > 0 && normalCount > 0) {
            g_splitCalls++;

            if (Config::Get().debugLogging) {
                SKSE::log::info("Splitting: {} (quest={}, normal={})",
                    a_entry->object ? a_entry->object->GetName() : "null",
                    questCount, normalCount);
            }

            auto* questEntry = new RE::InventoryEntryData(a_entry->object, questCount);
            questEntry->extraLists = new RE::BSSimpleList<RE::ExtraDataList*>();
            for (auto* xList : questLists) {
                questEntry->extraLists->push_front(xList);
            }

            auto* normalEntry = new RE::InventoryEntryData(a_entry->object, normalCount);
            normalEntry->extraLists = new RE::BSSimpleList<RE::ExtraDataList*>();
            for (auto* xList : normalLists) {
                normalEntry->extraLists->push_front(xList);
            }

            g_originalAddToItemList(a_itemList, questEntry, a_param3);

            return g_originalAddToItemList(a_itemList, normalEntry, a_param3);
        }

        return g_originalAddToItemList(a_itemList, a_entry, a_param3);
    }

    // ============================================================
    // INSTALLATION
    // ============================================================

    static void* CreateManualHook(
        std::uintptr_t targetAddr,
        void* hookFunc,
        size_t prologueSize
    ) {
        auto& trampoline = SKSE::GetTrampoline();
        auto* bytes = reinterpret_cast<std::uint8_t*>(targetAddr);

        auto* trampolineCode = trampoline.allocate(32);

        std::memcpy(trampolineCode, bytes, prologueSize);

        std::uintptr_t returnAddr = targetAddr + prologueSize;
        std::uint8_t* jumpBack = reinterpret_cast<std::uint8_t*>(trampolineCode) + prologueSize;
        jumpBack[0] = 0xFF;  // JMP [rip+0]
        jumpBack[1] = 0x25;
        jumpBack[2] = 0x00;
        jumpBack[3] = 0x00;
        jumpBack[4] = 0x00;
        jumpBack[5] = 0x00;
        std::memcpy(jumpBack + 6, &returnAddr, 8);

        auto* jumpStub = trampoline.allocate(14);
        auto hookAddr = reinterpret_cast<std::uintptr_t>(hookFunc);

        std::uint8_t* stubBytes = reinterpret_cast<std::uint8_t*>(jumpStub);
        stubBytes[0] = 0xFF;
        stubBytes[1] = 0x25;
        stubBytes[2] = 0x00;
        stubBytes[3] = 0x00;
        stubBytes[4] = 0x00;
        stubBytes[5] = 0x00;
        std::memcpy(stubBytes + 6, &hookAddr, 8);

        auto stubAddr = reinterpret_cast<std::uintptr_t>(jumpStub);
        std::int32_t relOffset = static_cast<std::int32_t>(stubAddr - targetAddr - 5);

        std::vector<std::uint8_t> patchBytes(prologueSize);
        patchBytes[0] = 0xE9;
        patchBytes[1] = static_cast<std::uint8_t>(relOffset);
        patchBytes[2] = static_cast<std::uint8_t>(relOffset >> 8);
        patchBytes[3] = static_cast<std::uint8_t>(relOffset >> 16);
        patchBytes[4] = static_cast<std::uint8_t>(relOffset >> 24);
        for (size_t i = 5; i < prologueSize; ++i) {
            patchBytes[i] = 0x90;  // NOP
        }

        REL::safe_write(targetAddr, patchBytes.data(), patchBytes.size());

        return trampolineCode;
    }

    void AddToItemListHook::Install() {
        Config::Get().Load();
        if (Config::Get().debugLogging) {
            SKSE::log::info("Debug logging enabled");
        }

        // AddToItemList hook
        // AE (1.6.x)   offset: 0x8ef050
        // SE (1.5.97)   offset: 0x856050
        // VR (1.4.15)   offset: 0x880410
        // Prologue: 40 56 57 41 56 (5 bytes)
        {
            auto baseAddr = REL::Module::get().base();
            std::uintptr_t offset = 0;

            if (REL::Module::IsAE()) {
                offset = 0x8ef050;
                SKSE::log::info("Detected AE runtime (version {})",
                    REL::Module::get().version().string());
            } else if (REL::Module::IsSE()) {
                offset = 0x856050;
                SKSE::log::info("Detected SE runtime (version {})",
                    REL::Module::get().version().string());
            } else if (REL::Module::IsVR()) {
                offset = 0x880410;
                SKSE::log::info("Detected VR runtime (version {})",
                    REL::Module::get().version().string());
            } else {
                SKSE::log::error("Unsupported runtime version: {}",
                    REL::Module::get().version().string());
                return;
            }

            std::uintptr_t funcAddr = baseAddr + offset;
            constexpr size_t PROLOGUE_SIZE = 5;

            g_originalAddToItemList = reinterpret_cast<AddToItemList_t>(
                CreateManualHook(funcAddr, reinterpret_cast<void*>(&HookedAddToItemList), PROLOGUE_SIZE)
            );

            SKSE::log::info("AddToItemList hooked at base+0x{:X}", offset);
        }

        if (Config::Get().debugLogging) {
            SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
                if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
                    if (auto ui = RE::UI::GetSingleton()) {
                        ui->AddEventSink(MenuEventHandler::GetSingleton());
                    }
                }
            });
        }
    }

}
