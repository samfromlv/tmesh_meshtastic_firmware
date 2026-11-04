#pragma once

#include <stdint.h>

namespace FeatureFlags
{

enum class RestrictedRoutingMode : uint8_t {
    STANDARD_CORE_PORTS,
    TEXT_COMPRESSED_TEXT_AND_ADMIN,
    TEXT_COMPRESSED_TEXT_ADMIN_AND_ROUTING,
    TEXT_COMPRESSED_TEXT_ADMIN_ROUTING_AND_TRACEROUTE,
    TEXT_COMPRESSED_TEXT_NODEINFO_ADMIN_AND_ROUTING,
    TEXT_COMPRESSED_TEXT_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE,
};

enum class HopDecrementMode : uint8_t {
    STANDARD_FAVORITE_ROUTER_BEHAVIOR,
    PRESERVE_AFTER_ANY_FAVORITE_RELAY,
    DECREMENT_FIRST_HOP_ONLY,
};

enum class ForceNextHopMode : uint8_t {
    FORCE_NEXT_HOP_DISABLED,
    LOCAL_PACKETS_WITH_FLOODING_FALLBACK,
    LOCAL_AND_RELAYED_PACKETS_WITH_FLOODING_FALLBACK,
    ALL_PACKETS_WITHOUT_FORCED_FLOODING_FALLBACK,
};

enum class FavoriteRouterMode : uint8_t {
    FAVORITE_ROUTER_DISABLED,
    PRESERVE_HOPS_ONLY,
    PRESERVE_HOPS_AND_RELAY,
};

void initialize();
RestrictedRoutingMode restrictedRoutingMode();
HopDecrementMode hopDecrementMode();
ForceNextHopMode forceNextHopMode();
uint8_t forceNextHopRelay();
FavoriteRouterMode favoriteRouterMode();
uint8_t favoriteRouterRelay();
bool isTmesh();
bool allowsTracerouteRouting(bool isMqttPacket);

} // namespace FeatureFlags