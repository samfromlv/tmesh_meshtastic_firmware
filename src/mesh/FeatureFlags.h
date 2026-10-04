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

void initialize();
RestrictedRoutingMode restrictedRoutingMode();
bool isTmesh();
bool allowsTracerouteRouting(bool isMqttPacket);

} // namespace FeatureFlags