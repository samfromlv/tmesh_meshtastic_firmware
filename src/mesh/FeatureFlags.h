#pragma once

#include "tmesh/feature_flags.pb.h"
#include <stddef.h>
#include <stdint.h>

class NodeDB;

namespace FeatureFlags
{

enum class RestrictedRoutingMode : uint8_t {
    STANDARD_CORE_PORTS,
    TEXT_ALL_AND_ADMIN,
    TEXT_ALL_ADMIN_AND_ROUTING,
    TEXT_ALL_ADMIN_ROUTING_AND_TRACEROUTE,
    TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING,
    TEXT_ALL_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE,
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

struct RestrictedRoutingConfig {
    RestrictedRoutingMode mode;
    bool allowsPrivateText;
};
struct ForceNextHopConfig {
    ForceNextHopMode mode;
    uint8_t relay;
};
struct FavoriteRouterConfig {
    FavoriteRouterMode mode;
    uint8_t relay;
};

typedef enum _meshtastic_ExtraPortNum : uint16_t { meshtastic_ExtraPortNum_PRIVATE_TEXT_APP = 283 } meshtastic_ExtraPortNum;

void initialize();
void loadConfig(NodeDB &db);
void resetConfig();
bool copyConfig(tmesh_FeatureFlagsConfig &out);
bool configAvailable();
tmesh_Response_Status setConfig(NodeDB &db, const tmesh_FeatureFlagsConfig *config);
void parseLegacyName(const char *name, size_t capacity, tmesh_FeatureFlagsConfig &out);
extern const char *const configFileName;
RestrictedRoutingConfig restrictedRoutingConfig();
ForceNextHopConfig forceNextHopConfig();
FavoriteRouterConfig favoriteRouterConfig();
RestrictedRoutingMode restrictedRoutingMode();
bool restrictedRoutingAllowsPrivateText();
HopDecrementMode hopDecrementMode();
ForceNextHopMode forceNextHopMode();
uint8_t forceNextHopRelay();
FavoriteRouterMode favoriteRouterMode();
uint8_t favoriteRouterRelay();
bool isTmesh();
bool allowsTracerouteRouting(bool isMqttPacket);

} // namespace FeatureFlags