#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "oot_engine.h"
#include "oot_hooks.h"
#include "oot_items.h"
#include "oot_layout_id.h"
#include "oot_registry.h"
#include "oot_resources.h"
#include "package_assets.h"
#include "linkspan/nei/nei_items.h"
#include "z64.h"

namespace {

constexpr const char* kOwner = "linkspan.shovel-demo";
constexpr const char* kItem = "linkspan.shovel-demo.shovel";
constexpr uint8_t kNoItem = 0xFF;

// Ícone e modelo da pá vêm da pasta assets/ do próprio pacote (mesmos caminhos do fork NEI, que as display lists
// citam por nome). A escala é a que o NEI usa para a pá no get-item.
constexpr const char* kPackIcon = "textures/icon_item_custom/gItemIconShovelTex";
constexpr const char* kPackModel = "objects/object_nei_shovel/gShovelGiveDL_opaque_dl";
constexpr float kPackModelScale = 0.2f;
// Reserva: o Martelo Megaton do oot.o2r (o manifesto exige esses dois arquivos), usado só se a pasta assets/ do pacote
// não puder ser montada ou não trouxer a pá.
constexpr const char* kFallbackIcon = "textures/icon_item_static/gItemIconHammerTex";
constexpr const char* kFallbackModel = "objects/object_gi_hammer/gGiHammerDL";
constexpr float kFallbackModelScale = 0.1f;

// Resultado da montagem de assets/, devolvido por "stats" (e, por ele, registrado no log pelo main.lua).
constexpr const char* kAssetsPackage = "package";   // montado: o item usa a pá do pacote
constexpr const char* kAssetsNoService = "noservice"; // host sem o serviço de recursos v2
constexpr const char* kAssetsNoFolder = "nofolder";   // pasta assets/ ao lado de provider/ ausente
constexpr const char* kAssetsMountFailed = "mountfail";
constexpr const char* kAssetsIncomplete = "incomplete"; // montou, mas ícone ou modelo não aparecem no VFS

struct Demo {
    const ShipOotEngineV1* engine = nullptr;
    const ShipOotMovementV1* movement = nullptr;
    const ShipOotItemsV2* items = nullptr;
    const ShipOotResourcesV2* resources = nullptr;
    const NeiItemsV1* nei = nullptr;
    uint64_t neiShovel = 0;
    uint64_t assets = 0;
    const char* assetsStatus = kAssetsNoService;
    uint8_t item = kNoItem;
    bool offered = false;
    uint32_t attempts = 0;
    uint32_t digs = 0;
    uint32_t refused = 0;
    uint32_t spawnFailures = 0;
};

ShipNativeStatus SHIP_NATIVE_CALL Use(void* user, uint8_t, uint8_t) {
    auto& demo = *static_cast<Demo*>(user);
    ++demo.attempts;
    if (!demo.movement->is_player_grounded()) {
        ++demo.refused;
        return SHIP_NATIVE_UNSUPPORTED;
    }
    auto* player = static_cast<Player*>(demo.engine->get_player());
    if (!player) return SHIP_NATIVE_UNSUPPORTED;
    const double angle = player->actor.shape.rot.y * (3.14159265358979323846 / 32768.0);
    const float x = player->actor.world.pos.x + static_cast<float>(std::sin(angle) * 55.0);
    const float z = player->actor.world.pos.z + static_cast<float>(std::cos(angle) * 55.0);
    if (!demo.engine->spawn_actor(ACTOR_EN_ITEM00, x, player->actor.world.pos.y + 15.0f, z,
                                  0, 0, 0, ITEM00_RUPEE_GREEN)) {
        ++demo.spawnFailures;
        return SHIP_NATIVE_FAILURE;
    }
    ++demo.digs;
    return SHIP_NATIVE_OK;
}

ShipNativeStatus SHIP_NATIVE_CALL Receive(void* user, uint8_t item) {
    auto& demo = *static_cast<Demo*>(user);
    for (uint8_t button = LINKSPAN_OOT_ITEMS_BUTTON_C_LEFT;
         button <= LINKSPAN_OOT_ITEMS_BUTTON_C_RIGHT; ++button) {
        uint8_t current = kNoItem;
        if (demo.items->get_button_item(button, &current) == SHIP_NATIVE_OK && current == kNoItem)
            return demo.items->set_button_item(button, item);
    }
    return demo.items->set_button_item(LINKSPAN_OOT_ITEMS_BUTTON_C_RIGHT, item);
}

ShipNativeStatus SHIP_NATIVE_CALL OnFrame(void* user, const ShipNativeHookCall* call) {
    auto& demo = *static_cast<Demo*>(user);
    const auto* play = static_cast<const PlayState*>(
        static_cast<const ShipOotPlayHookV1*>(call->payload)->play_state);
    if (!play || !demo.engine->get_player() || play->state.frames < 240 ||
        play->state.frames % 20 != 0) return SHIP_NATIVE_OK;
    if (demo.neiShovel) {
        NeiItemStateV1 state{sizeof(state)};
        if (demo.nei->get_state(demo.neiShovel, &state) != SHIP_NATIVE_OK) return SHIP_NATIVE_OK;
        if (!state.owned && demo.nei->grant_item(demo.neiShovel) != SHIP_NATIVE_OK) return SHIP_NATIVE_OK;
        // Saves antigos podem ter a pá provisória do demo num botão. Troque somente essa cópia.
        uint8_t firstEmpty = 0;
        for (uint8_t button = LINKSPAN_OOT_ITEMS_BUTTON_C_LEFT;
             button <= LINKSPAN_OOT_ITEMS_BUTTON_C_RIGHT; ++button) {
            uint8_t current = kNoItem;
            if (demo.items->get_button_item(button, &current) != SHIP_NATIVE_OK) continue;
            if (current == demo.item) {
                demo.items->set_button_item(button, kNoItem);
                demo.nei->equip(demo.neiShovel, button);
                firstEmpty = 0;
                break;
            }
            if (current == kNoItem && !firstEmpty) firstEmpty = button;
        }
        if (!state.owned && firstEmpty) demo.nei->equip(demo.neiShovel, firstEmpty);
        demo.offered = true;
        return SHIP_NATIVE_OK;
    }
    if (demo.offered) return SHIP_NATIVE_OK;
    for (uint8_t button = LINKSPAN_OOT_ITEMS_BUTTON_C_LEFT;
         button <= LINKSPAN_OOT_ITEMS_BUTTON_C_RIGHT; ++button) {
        uint8_t current = kNoItem;
        if (demo.items->get_button_item(button, &current) == SHIP_NATIVE_OK && current == demo.item) {
            demo.offered = true;
            return SHIP_NATIVE_OK;
        }
    }
    if (demo.items->give_item(demo.item) == SHIP_NATIVE_OK) demo.offered = true;
    return SHIP_NATIVE_OK;
}

ShipNativeStatus SHIP_NATIVE_CALL Stats(void* user, const char*, uint32_t length,
                                        ShipNativeWriteFn write, void* writer) {
    if (length) return SHIP_NATIVE_INVALID_ARGUMENT;
    const auto& demo = *static_cast<const Demo*>(user);
    NeiItemStateV1 neiState{sizeof(neiState)};
    const bool neiOwned = demo.neiShovel &&
        demo.nei->get_state(demo.neiShovel, &neiState) == SHIP_NATIVE_OK && neiState.owned;
    char output[256];
    const int size = std::snprintf(output, sizeof(output),
                                   "item=0x%02X assets=%s nei=%u neiOwned=%u offered=%u attempts=%u digs=%u refused=%u spawnFailures=%u",
                                   demo.item, demo.assetsStatus, demo.neiShovel ? 1u : 0u, neiOwned ? 1u : 0u,
                                   demo.offered ? 1u : 0u, demo.attempts, demo.digs,
                                   demo.refused, demo.spawnFailures);
    if (size < 0 || size >= static_cast<int>(sizeof(output))) return SHIP_NATIVE_FAILURE;
    return write(writer, output, static_cast<uint32_t>(size));
}

// Monta a pasta assets/ do pacote e confere que a pá está nela. Devolve true com a pá do pacote pronta; em qualquer
// falha deixa o motivo em demo.assetsStatus e o item cai no Martelo.
bool MountPackageAssets(Demo& demo) {
    if (!demo.resources || demo.resources->size < sizeof(ShipOotResourcesV2) || !demo.resources->has_file ||
        !demo.resources->mount_archive || !demo.resources->unmount_archive) {
        demo.assetsStatus = kAssetsNoService;
        return false;
    }
    std::string folder;
    try {
        folder = ProviderAssetsDirectory();
    } catch (...) {
    }
    if (folder.empty()) {
        demo.assetsStatus = kAssetsNoFolder;
        return false;
    }
    uint64_t handle = 0;
    if (demo.resources->mount_archive(folder.c_str(), &handle) != SHIP_NATIVE_OK || !handle) {
        demo.assetsStatus = kAssetsMountFailed;
        return false;
    }
    if (!demo.resources->has_file(kPackIcon) || !demo.resources->has_file(kPackModel)) {
        demo.resources->unmount_archive(handle);
        demo.assetsStatus = kAssetsIncomplete;
        return false;
    }
    demo.assets = handle;
    demo.assetsStatus = kAssetsPackage;
    return true;
}

void Release(Demo* demo) {
    if (!demo) return;
    if (demo->items && demo->item != kNoItem) demo->items->unregister_item(demo->item);
    // Depois do item: nenhum botão nem get-item aponta mais para o ícone e o modelo montados.
    if (demo->resources && demo->assets) demo->resources->unmount_archive(demo->assets);
    delete demo;
}

ShipNativeStatus SHIP_NATIVE_CALL Init(const ShipNativeRuntime* runtime, void** instance) {
    if (!runtime || !instance || runtime->size < sizeof(ShipNativeRuntime) || runtime->abi_minor < 2 ||
        !runtime->get_service || !runtime->register_hook || !runtime->register_function)
        return SHIP_NATIVE_UNSUPPORTED;
    const auto service = [runtime](const char* name, uint32_t version, uint32_t size) {
        return runtime->get_service(runtime->context, name, version, size);
    };
    const auto* engine = static_cast<const ShipOotEngineV1*>(
        service(LINKSPAN_OOT_ENGINE_SERVICE, LINKSPAN_OOT_ENGINE_VERSION, sizeof(ShipOotEngineV1)));
    const auto* movement = static_cast<const ShipOotMovementV1*>(
        service(LINKSPAN_OOT_MOVEMENT_SERVICE, LINKSPAN_OOT_MOVEMENT_VERSION, sizeof(ShipOotMovementV1)));
    const auto* items = static_cast<const ShipOotItemsV2*>(
        service(LINKSPAN_OOT_ITEMS_SERVICE, LINKSPAN_OOT_ITEMS_VERSION_2, sizeof(ShipOotItemsV2)));
    const auto* registry = static_cast<const ShipOotRegistryV2*>(
        service(LINKSPAN_OOT_REGISTRY_SERVICE, LINKSPAN_OOT_REGISTRY_VERSION_2, sizeof(ShipOotRegistryV2)));
    if (!engine || !movement || !items || !registry || !engine->layout_id ||
        std::strcmp(engine->layout_id, LINKSPAN_OOT_LAYOUT_ID) != 0) return SHIP_NATIVE_UNSUPPORTED;
    auto* demo = new (std::nothrow) Demo;
    if (!demo) return SHIP_NATIVE_FAILURE;
    demo->engine = engine;
    demo->movement = movement;
    demo->items = items;
    // O serviço de recursos é opcional: sem ele (ou sem a pasta assets/), o item usa o Martelo Megaton.
    demo->resources = static_cast<const ShipOotResourcesV2*>(
        service(LINKSPAN_OOT_RESOURCES_SERVICE, LINKSPAN_OOT_RESOURCES_VERSION_2, sizeof(ShipOotResourcesV2)));
    const bool packAssets = MountPackageAssets(*demo);
    const char* const icon = packAssets ? kPackIcon : kFallbackIcon;
    const char* const model = packAssets ? kPackModel : kFallbackModel;
    const float modelScale = packAssets ? kPackModelScale : kFallbackModelScale;
    demo->nei = static_cast<const NeiItemsV1*>(service(LINKSPAN_NEI_ITEMS_SERVICE,
                                                        LINKSPAN_NEI_ITEMS_VERSION, sizeof(NeiItemsV1)));
    if (demo->nei) demo->nei->find_item("skijer.nei.shovel", &demo->neiShovel);
    uint64_t space = 0;
    uint64_t entry = 0;
    int32_t assigned = 0;
    ShipNativeStatus status = registry->create_space_owned(kOwner, "linkspan-demo/tools", 1, 32, 1, &space);
    if (status == SHIP_NATIVE_OK)
        status = registry->register_entry_owned(kOwner, space, kItem, LINKSPAN_OOT_REGISTRY_AUTO_ID,
                                                nullptr, 0, &entry, &assigned);
    if (status == SHIP_NATIVE_OK) {
        const ShipOotItemSpecV1 spec{sizeof(ShipOotItemSpecV1), kItem, icon,
                                     LINKSPAN_OOT_ITEM_AGE_CHILD, Use, demo};
        status = items->register_item(&spec, &demo->item);
    }
    if (status == SHIP_NATIVE_OK) {
        const ShipOotGetItemSpecV1 spec{sizeof(ShipOotGetItemSpecV1), model,
                                        LINKSPAN_OOT_ITEMS_LAYER_OPAQUE, modelScale,
                                        "You got the %rTest Shovel%w!&Use it on firm ground.", Receive, demo};
        status = items->set_get_item(demo->item, &spec);
    }
    if (status == SHIP_NATIVE_OK) {
        const ShipNativeHookSpec spec{sizeof(ShipNativeHookSpec), LINKSPAN_OOT_HOOK_PLAY_UPDATE,
                                      LINKSPAN_OOT_HOOKS_VERSION, sizeof(ShipOotPlayHookV1),
                                      SHIP_NATIVE_HOOK_OBSERVE, SHIP_NATIVE_HOOK_AFTER, 0, OnFrame, demo};
        uint64_t handle = 0;
        status = runtime->register_hook(runtime->context, &spec, &handle);
    }
    if (status == SHIP_NATIVE_OK)
        status = runtime->register_function(runtime->context, "stats", Stats, demo);
    if (status != SHIP_NATIVE_OK) {
        Release(demo);
        return status;
    }
    *instance = demo;
    return SHIP_NATIVE_OK;
}

void SHIP_NATIVE_CALL Shutdown(void* instance) { Release(static_cast<Demo*>(instance)); }

} // namespace

extern "C" SHIP_NATIVE_EXPORT const ShipNativeDescriptor* SHIP_NATIVE_CALL ShipNative_Query(void) {
    static const ShipNativeDescriptor descriptor{sizeof(ShipNativeDescriptor), SHIP_NATIVE_ABI_MAJOR,
                                                 2u, Init, Shutdown};
    return &descriptor;
}
