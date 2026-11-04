#include "FeatureFlags.h"

#include "DebugConfiguration.h"
#include "Default.h"
#include "NodeDB.h"

#include <string.h>

namespace FeatureFlags
{

static RestrictedRoutingMode cachedRestrictedRoutingMode = RestrictedRoutingMode::STANDARD_CORE_PORTS;
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

void initialize()
{
    cachedRestrictedRoutingMode = RestrictedRoutingMode::STANDARD_CORE_PORTS;
    cachedHopDecrementMode = HopDecrementMode::STANDARD_FAVORITE_ROUTER_BEHAVIOR;
    cachedForceNextHopMode = ForceNextHopMode::FORCE_NEXT_HOP_DISABLED;
    cachedForceNextHopRelay = 0;
    cachedFavoriteRouterMode = FavoriteRouterMode::FAVORITE_ROUTER_DISABLED;
    cachedFavoriteRouterRelay = 0;
    cachedIsTmesh = moduleConfig.mqtt.enabled && strstr(moduleConfig.mqtt.address, tmesh_mqtt_address_part) != nullptr;

    const char *name = moduleConfig.detection_sensor.name;
    if (!moduleConfig.detection_sensor.enabled && name[0] == '_' && name[1] == 't') {
        const size_t capacity = sizeof(moduleConfig.detection_sensor.name);
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
                        const ForceNextHopMode modes[] = {
                            ForceNextHopMode::LOCAL_PACKETS_WITH_FLOODING_FALLBACK,
                            ForceNextHopMode::LOCAL_AND_RELAYED_PACKETS_WITH_FLOODING_FALLBACK,
                            ForceNextHopMode::ALL_PACKETS_WITHOUT_FORCED_FLOODING_FALLBACK,
                        };
                        cachedForceNextHopMode = modes[mode - '0'];
                        cachedForceNextHopRelay = static_cast<uint8_t>((high << 4) | low);
                    } else {
                        cachedFavoriteRouterMode =
                            mode == '0' ? FavoriteRouterMode::PRESERVE_HOPS_ONLY : FavoriteRouterMode::PRESERVE_HOPS_AND_RELAY;
                        cachedFavoriteRouterRelay = static_cast<uint8_t>((high << 4) | low);
                    }
                }
            } else if (length > 0 && (name[start] == 'r' || name[start] == 'h') && length != 2) {
                LOG_WARN("Invalid feature flag length for %c", name[start]);
            } else if (length == 2 && name[start] == 'r') {
                switch (name[start + 1]) {
                case '1':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_AND_ADMIN;
                    break;
                case '2':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_ADMIN_AND_ROUTING;
                    break;
                case '3':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_ADMIN_ROUTING_AND_TRACEROUTE;
                    break;
                case '4':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_NODEINFO_ADMIN_AND_ROUTING;
                    break;
                case '5':
                    cachedRestrictedRoutingMode =
                        RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE;
                    break;
                default:
                    LOG_WARN("Invalid restricted routing feature flag value %c", name[start + 1]);
                    break;
                }
            } else if (length == 2 && name[start] == 'h') {
                switch (name[start + 1]) {
                case '0':
                    cachedHopDecrementMode = HopDecrementMode::PRESERVE_AFTER_ANY_FAVORITE_RELAY;
                    break;
                case '1':
                    cachedHopDecrementMode = HopDecrementMode::DECREMENT_FIRST_HOP_ONLY;
                    break;
                default:
                    LOG_WARN("Invalid hop decrement feature flag value %c", name[start + 1]);
                    break;
                }
            }
            if (name[index] == '_')
                index++;
        }
    }

    LOG_DEBUG("Feature flags initialized: restricted routing mode %u, hop decrement mode %u",
              (unsigned)cachedRestrictedRoutingMode, (unsigned)cachedHopDecrementMode);
    LOG_DEBUG("Feature flags: force next hop mode %u relay 0x%x, favorite router mode %u relay 0x%x",
              (unsigned)cachedForceNextHopMode, cachedForceNextHopRelay, (unsigned)cachedFavoriteRouterMode,
              cachedFavoriteRouterRelay);
}

RestrictedRoutingMode restrictedRoutingMode()
{
    return cachedRestrictedRoutingMode;
}

HopDecrementMode hopDecrementMode()
{
    return cachedHopDecrementMode;
}

ForceNextHopMode forceNextHopMode()
{
    return cachedForceNextHopMode;
}

uint8_t forceNextHopRelay()
{
    return cachedForceNextHopRelay;
}

FavoriteRouterMode favoriteRouterMode()
{
    return cachedFavoriteRouterMode;
}

uint8_t favoriteRouterRelay()
{
    return cachedFavoriteRouterRelay;
}

bool isTmesh()
{
    return cachedIsTmesh;
}

bool allowsTracerouteRouting(bool isMqttPacket)
{
    return (cachedIsTmesh && isMqttPacket) || cachedRestrictedRoutingMode == RestrictedRoutingMode::STANDARD_CORE_PORTS ||
           cachedRestrictedRoutingMode == RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE ||
           cachedRestrictedRoutingMode == RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_ADMIN_ROUTING_AND_TRACEROUTE;
}

} // namespace FeatureFlags