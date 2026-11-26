#include "FeatureFlags.h"

#include "DebugConfiguration.h"
#include "Default.h"
#include "FSCommon.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "concurrency/Lock.h"

#include <string.h>

namespace FeatureFlags
{

const char *const configFileName = "/prefs/tmesh-flags.proto";
static concurrency::Lock cacheMutex;
static concurrency::Lock configMutex;
static tmesh_Store store = tmesh_Store_init_zero;
static bool configLoadFailed = false;

static_assert(static_cast<int>(RestrictedRoutingMode::TEXT_ALL_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE) ==
              _tmesh_FeatureFlagsConfig_RestrictedRoutingMode_MAX);
static_assert(static_cast<int>(HopDecrementMode::DECREMENT_FIRST_HOP_ONLY) == _tmesh_FeatureFlagsConfig_HopDecrementMode_MAX);
static_assert(static_cast<int>(ForceNextHopMode::ALL_PACKETS_WITHOUT_FORCED_FLOODING_FALLBACK) ==
              _tmesh_FeatureFlagsConfig_ForceNextHopMode_MAX);
static_assert(static_cast<int>(FavoriteRouterMode::PRESERVE_HOPS_AND_RELAY) == _tmesh_FeatureFlagsConfig_FavoriteRouterMode_MAX);

static bool validConfig(const tmesh_FeatureFlagsConfig &value)
{
    return value.restricted_routing_mode >= _tmesh_FeatureFlagsConfig_RestrictedRoutingMode_MIN &&
           value.restricted_routing_mode <= _tmesh_FeatureFlagsConfig_RestrictedRoutingMode_MAX &&
           value.hop_decrement_mode >= _tmesh_FeatureFlagsConfig_HopDecrementMode_MIN &&
           value.hop_decrement_mode <= _tmesh_FeatureFlagsConfig_HopDecrementMode_MAX &&
           value.force_next_hop_mode >= _tmesh_FeatureFlagsConfig_ForceNextHopMode_MIN &&
           value.force_next_hop_mode <= _tmesh_FeatureFlagsConfig_ForceNextHopMode_MAX &&
           value.favorite_router_mode >= _tmesh_FeatureFlagsConfig_FavoriteRouterMode_MIN &&
           value.favorite_router_mode <= _tmesh_FeatureFlagsConfig_FavoriteRouterMode_MAX &&
           value.force_next_hop_relay <= UINT8_MAX && value.favorite_router_relay <= UINT8_MAX &&
           (value.force_next_hop_mode == tmesh_FeatureFlagsConfig_ForceNextHopMode_FORCE_NEXT_HOP_DISABLED ||
            value.force_next_hop_relay != 0) &&
           (value.favorite_router_mode == tmesh_FeatureFlagsConfig_FavoriteRouterMode_FAVORITE_ROUTER_DISABLED ||
            value.favorite_router_relay != 0);
}

void resetConfig()
{
    concurrency::LockGuard guard(&configMutex);
    store = tmesh_Store_init_zero;
    configLoadFailed = false;
}

void loadConfig(NodeDB &db)
{
    concurrency::LockGuard guard(&configMutex);
    store = tmesh_Store_init_zero;
    configLoadFailed = false;
#ifdef FSCom
    spiLock->lock();
    const bool exists = FSCom.exists(configFileName);
    spiLock->unlock();
    if (!exists)
        return;
    tmesh_Store loaded = tmesh_Store_init_zero;
    if (db.loadProto(configFileName, tmesh_Store_size, sizeof(loaded), &tmesh_Store_msg, &loaded) !=
            LoadFileResult::LOAD_SUCCESS ||
        loaded.format_version != 1 || (loaded.has_config && !validConfig(loaded.config))) {
        configLoadFailed = true;
        LOG_ERROR("Unable to load valid TMesh feature flags; legacy flags suppressed");
        return;
    }
    store = loaded;
#else
    configLoadFailed = true;
    LOG_WARN("TMesh feature flags storage unavailable");
#endif
}

bool copyConfig(tmesh_FeatureFlagsConfig &out)
{
    concurrency::LockGuard guard(&configMutex);
    if (!store.has_config)
        return false;
    out = store.config;
    return true;
}

bool configAvailable()
{
    concurrency::LockGuard guard(&configMutex);
    return !configLoadFailed;
}

tmesh_Response_Status setConfig(NodeDB &db, const tmesh_FeatureFlagsConfig *value)
{
    if (value && !validConfig(*value)) {
        LOG_WARN("Invalid TMesh feature flags config");
        return tmesh_Response_Status_INVALID_ARGUMENT;
    }
    {
        concurrency::LockGuard guard(&configMutex);
        tmesh_Store next = tmesh_Store_init_zero;
        next.format_version = 1;
        if (value) {
            next.has_config = true;
            next.config = *value;
        }
        if (!db.saveProto(configFileName, tmesh_Store_size, &tmesh_Store_msg, &next)) {
            LOG_ERROR("Failed to save TMesh feature flags");
            return tmesh_Response_Status_STORAGE_ERROR;
        }
        store = next;
        configLoadFailed = false;
    }
    initialize();
    return tmesh_Response_Status_OK;
}

static RestrictedRoutingMode cachedRestrictedRoutingMode = RestrictedRoutingMode::STANDARD_CORE_PORTS;
static bool cachedRestrictedRoutingAllowsPrivateText = true;
static HopDecrementMode cachedHopDecrementMode = HopDecrementMode::STANDARD_FAVORITE_ROUTER_BEHAVIOR;
static ForceNextHopMode cachedForceNextHopMode = ForceNextHopMode::FORCE_NEXT_HOP_DISABLED;
static uint8_t cachedForceNextHopRelay;
static FavoriteRouterMode cachedFavoriteRouterMode = FavoriteRouterMode::FAVORITE_ROUTER_DISABLED;
static uint8_t cachedFavoriteRouterRelay;
static bool cachedIsTmesh;

static int hexDigit(char digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

void parseLegacyName(const char *name, size_t capacity, tmesh_FeatureFlagsConfig &out)
{
    out = tmesh_FeatureFlagsConfig_init_zero;
    for (size_t index = 2; index < capacity && name[index] != '\0';) {
        const size_t start = index;
        while (index < capacity && name[index] != '\0' && name[index] != '_')
            index++;
        const size_t length = index - start;
        if (index == capacity) {
            LOG_WARN("Unterminated feature flags");
            break;
        }
        if (length > 0 && (name[start] == 'n' || name[start] == 'f')) {
            const char prefix = name[start];
            if (length != 4) {
                LOG_WARN("Invalid feature flag length for %c", prefix);
            } else {
                const int high = hexDigit(name[start + 2]);
                const int low = hexDigit(name[start + 3]);
                const char mode = name[start + 1];
                if (high < 0 || low < 0 || (high == 0 && low == 0) || mode < '0' || mode > (prefix == 'n' ? '2' : '1')) {
                    LOG_WARN("Invalid mode or relay byte for feature flag %c", prefix);
                } else if (prefix == 'n') {
                    out.force_next_hop_mode = static_cast<tmesh_FeatureFlagsConfig_ForceNextHopMode>(mode - '0' + 1);
                    out.force_next_hop_relay = (high << 4) | low;
                } else {
                    out.favorite_router_mode = static_cast<tmesh_FeatureFlagsConfig_FavoriteRouterMode>(mode - '0' + 1);
                    out.favorite_router_relay = (high << 4) | low;
                }
            }
        } else if (length > 0 && name[start] == 'r') {
            const bool strict = length == 3 && name[start + 1] == 's';
            if (length != 2 && !strict) {
                LOG_WARN("Invalid feature flag length for r");
            } else {
                const char mode = name[start + (strict ? 2 : 1)];
                if (mode >= '0' && mode <= '5') {
                    out.restricted_routing_mode = static_cast<tmesh_FeatureFlagsConfig_RestrictedRoutingMode>(mode - '0');
                    out.restricted_routing_disallows_private_text = strict;
                } else {
                    LOG_WARN("Invalid restricted routing feature flag value %c", mode);
                }
            }
        } else if (length > 0 && name[start] == 'h') {
            if (length != 2) {
                LOG_WARN("Invalid feature flag length for h");
            } else if (name[start + 1] == '0' || name[start + 1] == '1') {
                out.hop_decrement_mode = static_cast<tmesh_FeatureFlagsConfig_HopDecrementMode>(name[start + 1] - '0' + 1);
            } else {
                LOG_WARN("Invalid hop decrement feature flag value %c", name[start + 1]);
            }
        }
        if (name[index] == '_')
            index++;
    }
}

void initialize()
{
    concurrency::LockGuard cacheGuard(&cacheMutex);
    cachedRestrictedRoutingMode = RestrictedRoutingMode::STANDARD_CORE_PORTS;
    cachedRestrictedRoutingAllowsPrivateText = true;
    cachedHopDecrementMode = HopDecrementMode::STANDARD_FAVORITE_ROUTER_BEHAVIOR;
    cachedForceNextHopMode = ForceNextHopMode::FORCE_NEXT_HOP_DISABLED;
    cachedForceNextHopRelay = 0;
    cachedFavoriteRouterMode = FavoriteRouterMode::FAVORITE_ROUTER_DISABLED;
    cachedFavoriteRouterRelay = 0;
    cachedIsTmesh = moduleConfig.mqtt.enabled && strstr(moduleConfig.mqtt.address, tmesh_mqtt_address_part) != nullptr;

    tmesh_Store current = tmesh_Store_init_zero;
    bool loadFailed;
    {
        concurrency::LockGuard configGuard(&configMutex);
        current = store;
        loadFailed = configLoadFailed;
    }
    const char *name = moduleConfig.detection_sensor.name;
    if (!current.has_config && !loadFailed && !moduleConfig.detection_sensor.enabled && name[0] == '_' && name[1] == 't') {
        parseLegacyName(name, sizeof(moduleConfig.detection_sensor.name), current.config);
        current.has_config = true;
    }
    if (current.has_config) {
        const auto &c = current.config;
        cachedRestrictedRoutingMode = static_cast<RestrictedRoutingMode>(c.restricted_routing_mode);
        cachedRestrictedRoutingAllowsPrivateText = !c.restricted_routing_disallows_private_text;
        cachedHopDecrementMode = static_cast<HopDecrementMode>(c.hop_decrement_mode);
        cachedForceNextHopMode = static_cast<ForceNextHopMode>(c.force_next_hop_mode);
        cachedForceNextHopRelay = static_cast<uint8_t>(c.force_next_hop_relay);
        cachedFavoriteRouterMode = static_cast<FavoriteRouterMode>(c.favorite_router_mode);
        cachedFavoriteRouterRelay = static_cast<uint8_t>(c.favorite_router_relay);
    }

    LOG_DEBUG("Feature flags initialized: restricted routing mode %u, private text %s, hop decrement mode %u",
              (unsigned)cachedRestrictedRoutingMode, cachedRestrictedRoutingAllowsPrivateText ? "allowed" : "disabled",
              (unsigned)cachedHopDecrementMode);
    LOG_DEBUG("Feature flags: force next hop mode %u relay 0x%x, favorite router mode %u relay 0x%x",
              (unsigned)cachedForceNextHopMode, cachedForceNextHopRelay, (unsigned)cachedFavoriteRouterMode,
              cachedFavoriteRouterRelay);
}

RestrictedRoutingMode restrictedRoutingMode()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedRestrictedRoutingMode;
}

RestrictedRoutingConfig restrictedRoutingConfig()
{
    concurrency::LockGuard guard(&cacheMutex);
    return {cachedRestrictedRoutingMode, cachedRestrictedRoutingAllowsPrivateText};
}

ForceNextHopConfig forceNextHopConfig()
{
    concurrency::LockGuard guard(&cacheMutex);
    return {cachedForceNextHopMode, cachedForceNextHopRelay};
}

FavoriteRouterConfig favoriteRouterConfig()
{
    concurrency::LockGuard guard(&cacheMutex);
    return {cachedFavoriteRouterMode, cachedFavoriteRouterRelay};
}

bool restrictedRoutingAllowsPrivateText()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedRestrictedRoutingAllowsPrivateText;
}

HopDecrementMode hopDecrementMode()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedHopDecrementMode;
}

ForceNextHopMode forceNextHopMode()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedForceNextHopMode;
}

uint8_t forceNextHopRelay()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedForceNextHopRelay;
}

FavoriteRouterMode favoriteRouterMode()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedFavoriteRouterMode;
}

uint8_t favoriteRouterRelay()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedFavoriteRouterRelay;
}

bool isTmesh()
{
    concurrency::LockGuard guard(&cacheMutex);
    return cachedIsTmesh;
}

bool allowsTracerouteRouting(bool isMqttPacket)
{
    concurrency::LockGuard guard(&cacheMutex);
    return (cachedIsTmesh && isMqttPacket) || cachedRestrictedRoutingMode == RestrictedRoutingMode::STANDARD_CORE_PORTS ||
           cachedRestrictedRoutingMode == RestrictedRoutingMode::TEXT_ALL_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE ||
           cachedRestrictedRoutingMode == RestrictedRoutingMode::TEXT_ALL_ADMIN_ROUTING_AND_TRACEROUTE;
}

} // namespace FeatureFlags