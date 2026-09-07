#include "FeatureFlags.h"

#include "DebugConfiguration.h"
#include "NodeDB.h"

namespace FeatureFlags {

static RestrictedRoutingMode cachedRestrictedRoutingMode = RestrictedRoutingMode::STANDARD_CORE_PORTS;

void initialize()
{
    const char *name = moduleConfig.detection_sensor.name;
    if (!moduleConfig.detection_sensor.enabled && name[0] == '_' && name[1] == 't') {
        for (size_t index = 2; index + 1 < sizeof(moduleConfig.detection_sensor.name) && name[index] != '\0'; index++) {
            if (name[index] == 'r') {
                switch (name[index + 1]) {
                case '1':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_AND_ADMIN;
                    break;
                case '2':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_ADMIN_AND_ROUTING;
                    break;
                case '3':
                    cachedRestrictedRoutingMode = RestrictedRoutingMode::TEXT_COMPRESSED_TEXT_ADMIN_ROUTING_AND_TRACEROUTE;
                    break;
                default:
                    continue;
                }
                break;
            }
        }
    }

    LOG_DEBUG("Feature flags initialized: restricted routing mode %u", (unsigned)cachedRestrictedRoutingMode);
}

RestrictedRoutingMode restrictedRoutingMode()
{
    return cachedRestrictedRoutingMode;
}

} // namespace FeatureFlags