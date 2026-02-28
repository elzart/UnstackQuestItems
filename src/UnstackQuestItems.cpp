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
        bool showZeroWeight = true;

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
            showZeroWeight = ini.GetBoolValue("General", "bShowZeroWeight", true);
        }
    };

    // ============================================================
    // GLOBALS
    // ============================================================

    static std::atomic<uint64_t> g_addToItemListCalls{0};
    static std::atomic<uint64_t> g_splitCalls{0};

    //   void* AddToItemList(void* itemList, RE::InventoryEntryData* entry, void* param3)
    using AddToItemList_t = void*(*)(void*, RE::InventoryEntryData*, void*);
    static AddToItemList_t g_originalAddToItemList = nullptr;

    using UpdateImpl_t = void(*)(RE::ItemList*, RE::TESObjectREFR*);
    static UpdateImpl_t g_originalUpdateImpl = nullptr;

    using SetItem_t = void(*)(RE::ItemCard*, const RE::InventoryEntryData*, bool);
    static SetItem_t g_originalSetItem = nullptr;

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

    static bool HasQuestExtraData(const RE::InventoryEntryData* a_entry) {
        if (!a_entry || !a_entry->extraLists) {
            return false;
        }
        for (auto* xList : *a_entry->extraLists) {
            if (IsQuestExtraDataList(xList)) {
                return true;
            }
        }
        return false;
    }

    // ============================================================
    // MENU HANDLER 
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
    // ZERO-WEIGHT DISPLAY FOR QUEST ITEMS
    // ============================================================

    static void HookedUpdateImpl(RE::ItemList* a_list, RE::TESObjectREFR* a_owner) {
        g_originalUpdateImpl(a_list, a_owner);

        for (auto* item : a_list->items) {
            if (!item) {
                continue;
            }
            if (HasQuestExtraData(item->data.objDesc)) {
                RE::GFxValue zero;
                zero.SetNumber(0.0);
                item->obj.SetMember("weight", zero);
            }
        }
    }

    static void HookedSetItem(
        RE::ItemCard* a_card,
        const RE::InventoryEntryData* a_item,
        bool a_ignoreStolen
    ) {
        g_originalSetItem(a_card, a_item, a_ignoreStolen);

        if (HasQuestExtraData(a_item)) {
            RE::GFxValue zero;
            zero.SetNumber(0.0);
            a_card->obj.SetMember("weight", zero);
        }
    }

    // ============================================================
    // INSTALLATION
    // ============================================================

    static void WriteAbsoluteJmp(std::uint8_t* dest, std::uintptr_t target) {
        dest[0] = 0xFF;
        dest[1] = 0x25;
        dest[2] = 0x00;
        dest[3] = 0x00;
        dest[4] = 0x00;
        dest[5] = 0x00;
        std::memcpy(dest + 6, &target, 8);
    }

    static void WriteRelativeJmp(std::uintptr_t targetAddr, std::uintptr_t stubAddr, size_t prologueSize) {
        std::int32_t relOffset = static_cast<std::int32_t>(stubAddr - targetAddr - 5);

        std::vector<std::uint8_t> patch(prologueSize, 0x90);
        patch[0] = 0xE9;
        patch[1] = static_cast<std::uint8_t>(relOffset);
        patch[2] = static_cast<std::uint8_t>(relOffset >> 8);
        patch[3] = static_cast<std::uint8_t>(relOffset >> 16);
        patch[4] = static_cast<std::uint8_t>(relOffset >> 24);

        REL::safe_write(targetAddr, patch.data(), patch.size());
    }

    static void* CreateManualHook(
        std::uintptr_t targetAddr,
        void* hookFunc,
        size_t prologueSize
    ) {
        auto& trampoline = SKSE::GetTrampoline();
        auto* bytes = reinterpret_cast<std::uint8_t*>(targetAddr);

        if (bytes[0] == 0xE9 && prologueSize >= 5) {
            std::int32_t rel32;
            std::memcpy(&rel32, bytes + 1, 4);
            std::uintptr_t prevTarget = targetAddr + 5 + rel32;

            SKSE::log::info("  existing hook detected — chaining (prev target: 0x{:X})", prevTarget);

            auto* trampolineCode = trampoline.allocate(14);
            WriteAbsoluteJmp(reinterpret_cast<std::uint8_t*>(trampolineCode), prevTarget);

            auto* jumpStub = trampoline.allocate(14);
            WriteAbsoluteJmp(
                reinterpret_cast<std::uint8_t*>(jumpStub),
                reinterpret_cast<std::uintptr_t>(hookFunc));

            WriteRelativeJmp(targetAddr, reinterpret_cast<std::uintptr_t>(jumpStub), prologueSize);
            return trampolineCode;
        }

        auto* trampolineCode = trampoline.allocate(32);

        std::memcpy(trampolineCode, bytes, prologueSize);

        std::uintptr_t returnAddr = targetAddr + prologueSize;
        WriteAbsoluteJmp(
            reinterpret_cast<std::uint8_t*>(trampolineCode) + prologueSize,
            returnAddr);

        auto* jumpStub = trampoline.allocate(14);
        WriteAbsoluteJmp(
            reinterpret_cast<std::uint8_t*>(jumpStub),
            reinterpret_cast<std::uintptr_t>(hookFunc));

        WriteRelativeJmp(targetAddr, reinterpret_cast<std::uintptr_t>(jumpStub), prologueSize);
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

        if (Config::Get().showZeroWeight) {
            // Prologue: 40 57 / 48 83 EC 30  (PUSH RDI; SUB RSP,0x30) = 6 bytes
            REL::Relocation<std::uintptr_t> updateImplAddr{ RELOCATION_ID(50099, 51031) };
            g_originalUpdateImpl = reinterpret_cast<UpdateImpl_t>(
                CreateManualHook(updateImplAddr.address(), reinterpret_cast<void*>(&HookedUpdateImpl), 6)
            );
            SKSE::log::info("ItemList::Update_Impl hooked for zero-weight display");

            // Prologue: 48 8B C4 / 44 88 40 18  (MOV RAX,RSP; MOV [RAX+18],R8B) = 7 bytes
            REL::Relocation<std::uintptr_t> setItemAddr{ RELOCATION_ID(51019, 51897) };
            g_originalSetItem = reinterpret_cast<SetItem_t>(
                CreateManualHook(setItemAddr.address(), reinterpret_cast<void*>(&HookedSetItem), 7)
            );
            SKSE::log::info("ItemCard::SetItem hooked for zero-weight display");
        } else {
            SKSE::log::info("Zero-weight display for quest items disabled");
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
