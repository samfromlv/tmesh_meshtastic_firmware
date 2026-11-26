// FeatureFlags in src/mesh/FeatureFlags.cpp must prefer the separate TMesh store over the
// detection-sensor encoding, survive reload, and return to the legacy encoding after CLEAR.
// AdminModule must translate qualifying detection-sensor updates into that store, preserving the
// existing token semantics and normal admin authorization, while deferring writes in edit transactions.
// These tests guard against ignored sensor edits, unintended changes from ordinary sensor edits,
// partial persistence, and unauthorized remote updates.

#include "MeshTypes.h"
#include "TestUtil.h"
#include "mesh/Channels.h"
#include "mesh/FeatureFlags.h"
#include "mesh/NodeDB.h"
#include "mesh/PhoneAPI.h"
#include "mesh/Router.h"
#include "modules/RoutingModule.h"
#include "support/AdminModuleTestShim.h"
#include "support/MockMeshService.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <unity.h>

static AdminModuleTestShim *admin;
static bool powerSafe = true;

#if !defined(_WIN32)
bool powerHAL_isPowerLevelSafe()
{
    return powerSafe;
}
#endif

void setUp()
{
    FeatureFlags::resetConfig();
    moduleConfig.detection_sensor.enabled = false;
    snprintf(moduleConfig.detection_sensor.name, sizeof(moduleConfig.detection_sensor.name), "_tr1_h0_n0ab_f0cd");
    FeatureFlags::initialize();
    config.security.is_managed = false;
    config.security.admin_key[0].size = 0;
    powerSafe = true;
    admin = new AdminModuleTestShim();
}

void tearDown()
{
    admin->drainReply();
    delete admin;
    FeatureFlags::resetConfig();
    FeatureFlags::initialize();
}

static meshtastic_ModuleConfig sensorConfig(const char *name, bool enabled = false)
{
    meshtastic_ModuleConfig sensor = meshtastic_ModuleConfig_init_zero;
    sensor.which_payload_variant = meshtastic_ModuleConfig_detection_sensor_tag;
    sensor.payload_variant.detection_sensor.enabled = enabled;
    snprintf(sensor.payload_variant.detection_sensor.name, sizeof(sensor.payload_variant.detection_sensor.name), "%s", name);
    return sensor;
}

static void dispatch(meshtastic_AdminMessage &message, meshtastic_MeshPacket &packet)
{
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.to = nodeDB->getNodeNum();
    packet.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    admin->handleReceivedProtobuf(packet, &message);
}

static void commitEdit()
{
    meshtastic_AdminMessage message = meshtastic_AdminMessage_init_zero;
    message.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
    message.commit_edit_settings = true;
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    dispatch(message, packet);
}

static void sendRequest(const tmesh_Request &request, tmesh_Response &response)
{
    tmesh_Envelope envelope = tmesh_Envelope_init_zero;
    envelope.which_payload = tmesh_Envelope_request_tag;
    envelope.payload.request = request;
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.from = 0;
    packet.to = nodeDB->getNodeNum();
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    packet.decoded.payload.size =
        pb_encode_to_bytes(packet.decoded.payload.bytes, sizeof(packet.decoded.payload.bytes), &tmesh_Envelope_msg, &envelope);
    TEST_ASSERT_GREATER_THAN(0, packet.decoded.payload.size);
    meshtastic_AdminMessage parsed = meshtastic_AdminMessage_init_zero;
    TEST_ASSERT_TRUE(
        pb_decode_from_bytes(packet.decoded.payload.bytes, packet.decoded.payload.size, &meshtastic_AdminMessage_msg, &parsed));
    TEST_ASSERT_TRUE(PhoneAPI::classifyLocalAdminPacket(packet, false, parsed) == PhoneAPI::LocalAdminGate::DropUnauthorized);
    TEST_ASSERT_TRUE(PhoneAPI::classifyLocalAdminPacket(packet, true, parsed) == PhoneAPI::LocalAdminGate::AuthorizedPassThrough);
    admin->handleReceivedProtobuf(packet, &parsed);
    TEST_ASSERT_NOT_NULL(admin->reply());
    tmesh_Envelope reply = tmesh_Envelope_init_zero;
    TEST_ASSERT_TRUE(pb_decode_from_bytes(admin->reply()->decoded.payload.bytes, admin->reply()->decoded.payload.size,
                                          &tmesh_Envelope_msg, &reply));
    TEST_ASSERT_EQUAL(tmesh_Envelope_response_tag, reply.which_payload);
    response = reply.payload.response;
    admin->drainReply();
}

void test_remote_authorized_admin_cannot_set_tmesh_flags()
{
    tmesh_Envelope envelope = tmesh_Envelope_init_zero;
    envelope.which_payload = tmesh_Envelope_request_tag;
    envelope.payload.request.operation = tmesh_Request_Operation_SET;
    envelope.payload.request.has_config = true;
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.from = 0x12345678;
    packet.to = nodeDB->getNodeNum();
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    packet.pki_encrypted = true;
    packet.public_key.size = 32;
    memset(packet.public_key.bytes, 0x42, 32);
    config.security.admin_key[0].size = 32;
    memcpy(config.security.admin_key[0].bytes, packet.public_key.bytes, 32);
    packet.decoded.payload.size =
        pb_encode_to_bytes(packet.decoded.payload.bytes, sizeof(packet.decoded.payload.bytes), &tmesh_Envelope_msg, &envelope);
    meshtastic_AdminMessage parsed = meshtastic_AdminMessage_init_zero;
    TEST_ASSERT_TRUE(
        pb_decode_from_bytes(packet.decoded.payload.bytes, packet.decoded.payload.size, &meshtastic_AdminMessage_msg, &parsed));
    TEST_ASSERT_TRUE(admin->handleReceivedProtobuf(packet, &parsed));
    TEST_ASSERT_NOT_NULL(admin->reply());
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    TEST_ASSERT_TRUE(pb_decode_from_bytes(admin->reply()->decoded.payload.bytes, admin->reply()->decoded.payload.size,
                                          &meshtastic_Routing_msg, &routing));
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_NOT_AUTHORIZED, routing.error_reason);
    tmesh_FeatureFlagsConfig out = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_FALSE(FeatureFlags::copyConfig(out));
}

void test_invalid_store_suppresses_legacy_and_reports_error()
{
    tmesh_Store invalid = tmesh_Store_init_zero;
    invalid.format_version = 2;
    TEST_ASSERT_TRUE(nodeDB->saveProto(FeatureFlags::configFileName, tmesh_Store_size, &tmesh_Store_msg, &invalid));
    FeatureFlags::loadConfig(*nodeDB);
    FeatureFlags::initialize();
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() == FeatureFlags::RestrictedRoutingMode::STANDARD_CORE_PORTS);
    tmesh_Request request = tmesh_Request_init_zero;
    request.operation = tmesh_Request_Operation_GET;
    tmesh_Response response = tmesh_Response_init_zero;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_STORAGE_ERROR, response.status);
}

void test_custom_config_overrides_legacy_and_survives_reload()
{
    tmesh_Request request = tmesh_Request_init_zero;
    request.operation = tmesh_Request_Operation_SET;
    request.has_config = true;
    request.config.restricted_routing_mode = tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING;
    request.config.restricted_routing_disallows_private_text = true;
    request.config.hop_decrement_mode = tmesh_FeatureFlagsConfig_HopDecrementMode_DECREMENT_FIRST_HOP_ONLY;
    request.config.force_next_hop_mode =
        tmesh_FeatureFlagsConfig_ForceNextHopMode_LOCAL_AND_RELAYED_PACKETS_WITH_FLOODING_FALLBACK;
    request.config.force_next_hop_relay = 0x42;
    request.config.favorite_router_mode = tmesh_FeatureFlagsConfig_FavoriteRouterMode_PRESERVE_HOPS_AND_RELAY;
    request.config.favorite_router_relay = 0x55;
    tmesh_Response response = tmesh_Response_init_zero;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, response.status);
    TEST_ASSERT_TRUE(response.has_config);
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() ==
                     FeatureFlags::RestrictedRoutingMode::TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING);
    TEST_ASSERT_FALSE(FeatureFlags::restrictedRoutingAllowsPrivateText());
    TEST_ASSERT_TRUE(FeatureFlags::hopDecrementMode() == FeatureFlags::HopDecrementMode::DECREMENT_FIRST_HOP_ONLY);
    TEST_ASSERT_TRUE(FeatureFlags::forceNextHopMode() ==
                     FeatureFlags::ForceNextHopMode::LOCAL_AND_RELAYED_PACKETS_WITH_FLOODING_FALLBACK);
    TEST_ASSERT_EQUAL_HEX8(0x42, FeatureFlags::forceNextHopRelay());
    TEST_ASSERT_TRUE(FeatureFlags::favoriteRouterMode() == FeatureFlags::FavoriteRouterMode::PRESERVE_HOPS_AND_RELAY);
    TEST_ASSERT_EQUAL_HEX8(0x55, FeatureFlags::favoriteRouterRelay());
    snprintf(moduleConfig.detection_sensor.name, sizeof(moduleConfig.detection_sensor.name), "_tr5");
    FeatureFlags::resetConfig();
    FeatureFlags::loadConfig(*nodeDB);
    FeatureFlags::initialize();
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() ==
                     FeatureFlags::RestrictedRoutingMode::TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING);
    request.operation = tmesh_Request_Operation_GET;
    request.has_config = false;
    sendRequest(request, response);
    TEST_ASSERT_TRUE(response.has_config);
    TEST_ASSERT_EQUAL_HEX8(0x42, response.config.force_next_hop_relay);
    request.operation = tmesh_Request_Operation_CLEAR;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, response.status);
    TEST_ASSERT_FALSE(response.has_config);
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() ==
                     FeatureFlags::RestrictedRoutingMode::TEXT_ALL_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE);
    FeatureFlags::resetConfig();
    FeatureFlags::loadConfig(*nodeDB);
    FeatureFlags::initialize();
    TEST_ASSERT_FALSE(FeatureFlags::copyConfig(response.config));
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() ==
                     FeatureFlags::RestrictedRoutingMode::TEXT_ALL_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE);
}

void test_invalid_config_does_not_change_active_values()
{
    tmesh_Request request = tmesh_Request_init_zero;
    request.operation = tmesh_Request_Operation_SET;
    request.has_config = true;
    request.config.force_next_hop_mode = tmesh_FeatureFlagsConfig_ForceNextHopMode_LOCAL_PACKETS_WITH_FLOODING_FALLBACK;
    request.config.force_next_hop_relay = 256;
    tmesh_Response response = tmesh_Response_init_zero;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_INVALID_ARGUMENT, response.status);
    TEST_ASSERT_FALSE(response.has_config);
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() == FeatureFlags::RestrictedRoutingMode::TEXT_ALL_AND_ADMIN);
    request.config.force_next_hop_relay = 0;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_INVALID_ARGUMENT, response.status);
}

void test_empty_custom_config_does_not_inherit_legacy_flags()
{
    tmesh_Request request = tmesh_Request_init_zero;
    request.operation = tmesh_Request_Operation_SET;
    request.has_config = true;
    tmesh_Response response = tmesh_Response_init_zero;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, response.status);
    TEST_ASSERT_TRUE(response.has_config);
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() == FeatureFlags::RestrictedRoutingMode::STANDARD_CORE_PORTS);
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingAllowsPrivateText());
    TEST_ASSERT_TRUE(FeatureFlags::hopDecrementMode() == FeatureFlags::HopDecrementMode::STANDARD_FAVORITE_ROUTER_BEHAVIOR);
    TEST_ASSERT_TRUE(FeatureFlags::forceNextHopMode() == FeatureFlags::ForceNextHopMode::FORCE_NEXT_HOP_DISABLED);
    TEST_ASSERT_EQUAL_HEX8(0, FeatureFlags::forceNextHopRelay());
    TEST_ASSERT_TRUE(FeatureFlags::favoriteRouterMode() == FeatureFlags::FavoriteRouterMode::FAVORITE_ROUTER_DISABLED);
    TEST_ASSERT_EQUAL_HEX8(0, FeatureFlags::favoriteRouterRelay());

    meshtastic_ModuleConfig sensor = meshtastic_ModuleConfig_init_zero;
    sensor.which_payload_variant = meshtastic_ModuleConfig_detection_sensor_tag;
    sensor.payload_variant.detection_sensor.enabled = true;
    snprintf(sensor.payload_variant.detection_sensor.name, sizeof(sensor.payload_variant.detection_sensor.name), "Motion");
    admin->deferSaves();
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensor));
    FeatureFlags::initialize();
    FeatureFlags::resetConfig();
    FeatureFlags::loadConfig(*nodeDB);
    FeatureFlags::initialize();
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(response.config));
    TEST_ASSERT_TRUE(FeatureFlags::restrictedRoutingMode() == FeatureFlags::RestrictedRoutingMode::STANDARD_CORE_PORTS);
}

void test_sensor_update_replaces_config_and_survives_reload()
{
    tmesh_FeatureFlagsConfig existing = tmesh_FeatureFlagsConfig_init_zero;
    existing.hop_decrement_mode = tmesh_FeatureFlagsConfig_HopDecrementMode_DECREMENT_FIRST_HOP_ONLY;
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, FeatureFlags::setConfig(*nodeDB, &existing));
    meshtastic_AdminMessage message = meshtastic_AdminMessage_init_zero;
    message.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
    message.set_module_config = sensorConfig("_trs3_n1aB_f1cd");
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    dispatch(message, packet);
    TEST_ASSERT_NULL(admin->reply());
    TEST_ASSERT_EQUAL_STRING("_trs3_n1aB_f1cd", moduleConfig.detection_sensor.name);
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    FeatureFlags::resetConfig();
    FeatureFlags::loadConfig(*nodeDB);
    FeatureFlags::initialize();
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_ADMIN_ROUTING_AND_TRACEROUTE,
                      stored.restricted_routing_mode);
    TEST_ASSERT_TRUE(stored.restricted_routing_disallows_private_text);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_HopDecrementMode_STANDARD_FAVORITE_ROUTER_BEHAVIOR, stored.hop_decrement_mode);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_ForceNextHopMode_LOCAL_AND_RELAYED_PACKETS_WITH_FLOODING_FALLBACK,
                      stored.force_next_hop_mode);
    TEST_ASSERT_EQUAL_HEX8(0xab, stored.force_next_hop_relay);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_FavoriteRouterMode_PRESERVE_HOPS_AND_RELAY, stored.favorite_router_mode);
    TEST_ASSERT_EQUAL_HEX8(0xcd, stored.favorite_router_relay);
}

void test_legacy_parser_ignores_invalid_tokens_and_last_valid_wins()
{
    const char name[] = "_tr2_h0_r9_n000_f1ff_r1_h1";
    tmesh_FeatureFlagsConfig flags = tmesh_FeatureFlagsConfig_init_zero;
    FeatureFlags::parseLegacyName(name, sizeof(name), flags);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_AND_ADMIN, flags.restricted_routing_mode);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_HopDecrementMode_DECREMENT_FIRST_HOP_ONLY, flags.hop_decrement_mode);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_ForceNextHopMode_FORCE_NEXT_HOP_DISABLED, flags.force_next_hop_mode);
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_FavoriteRouterMode_PRESERVE_HOPS_AND_RELAY, flags.favorite_router_mode);
    TEST_ASSERT_EQUAL_HEX8(0xff, flags.favorite_router_relay);
}

void test_ordinary_sensor_updates_leave_flags_untouched()
{
    tmesh_FeatureFlagsConfig existing = tmesh_FeatureFlagsConfig_init_zero;
    existing.restricted_routing_mode = tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_AND_ADMIN;
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, FeatureFlags::setConfig(*nodeDB, &existing));
    admin->deferSaves();
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("_tr5", true)));
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("Motion")));
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(existing.restricted_routing_mode, stored.restricted_routing_mode);
}

void test_transaction_defers_and_commits_last_matching_update()
{
    tmesh_FeatureFlagsConfig existing = tmesh_FeatureFlagsConfig_init_zero;
    existing.restricted_routing_mode = tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_AND_ADMIN;
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, FeatureFlags::setConfig(*nodeDB, &existing));
    admin->deferSaves();
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("_tr2")));
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("_tr5")));
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("Motion")));
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(existing.restricted_routing_mode, stored.restricted_routing_mode);
    FeatureFlags::loadConfig(*nodeDB);
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(existing.restricted_routing_mode, stored.restricted_routing_mode);
    commitEdit();
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
    FeatureFlags::loadConfig(*nodeDB);
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_NODEINFO_ADMIN_ROUTING_AND_TRACEROUTE,
                      stored.restricted_routing_mode);
}

void test_abandoned_transaction_persists_sensor_flags()
{
    admin->deferSaves();
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("_tr4")));
    admin->ageEditTransaction();
    meshtastic_AdminMessage message = meshtastic_AdminMessage_init_zero;
    message.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_request_tag;
    message.get_device_metadata_request = true;
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    dispatch(message, packet);
    admin->drainReply();
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
    FeatureFlags::resetConfig();
    FeatureFlags::loadConfig(*nodeDB);
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING,
                      stored.restricted_routing_mode);
}

void test_direct_tmesh_write_supersedes_deferred_sensor_update()
{
    admin->deferSaves();
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("_tr4")));
    tmesh_Request request = tmesh_Request_init_zero;
    request.operation = tmesh_Request_Operation_SET;
    request.has_config = true;
    request.config.restricted_routing_mode = tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_AND_ADMIN;
    tmesh_Response response = tmesh_Response_init_zero;
    sendRequest(request, response);
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, response.status);
    commitEdit();
    FeatureFlags::resetConfig();
    FeatureFlags::loadConfig(*nodeDB);
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_AND_ADMIN, stored.restricted_routing_mode);
}

void test_remote_sensor_update_requires_authorization_and_session()
{
    meshtastic_AdminMessage message = meshtastic_AdminMessage_init_zero;
    message.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
    message.set_module_config = sensorConfig("_tr4");
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.from = 0x12345678;
    packet.pki_encrypted = true;
    packet.public_key.size = 32;
    memset(packet.public_key.bytes, 0x42, 32);
    dispatch(message, packet);
    TEST_ASSERT_NOT_NULL(admin->reply());
    admin->drainReply();
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_FALSE(FeatureFlags::copyConfig(stored));
    config.security.admin_key[0].size = 32;
    memcpy(config.security.admin_key[0].bytes, packet.public_key.bytes, 32);
    dispatch(message, packet);
    TEST_ASSERT_NOT_NULL(admin->reply());
    admin->drainReply();
    TEST_ASSERT_FALSE(FeatureFlags::copyConfig(stored));
    meshtastic_AdminMessage key = meshtastic_AdminMessage_init_zero;
    admin->setPassKey(&key);
    message.session_passkey = key.session_passkey;
    dispatch(message, packet);
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING,
                      stored.restricted_routing_mode);
}

#if !defined(_WIN32)
void test_storage_failure_does_not_apply_sensor_or_flags()
{
    tmesh_FeatureFlagsConfig existing = tmesh_FeatureFlagsConfig_init_zero;
    existing.restricted_routing_mode = tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_AND_ADMIN;
    TEST_ASSERT_EQUAL(tmesh_Response_Status_OK, FeatureFlags::setConfig(*nodeDB, &existing));
    powerSafe = false;
    TEST_ASSERT_FALSE(admin->handleSetModuleConfig(sensorConfig("_tr4")));
    TEST_ASSERT_EQUAL_STRING("_tr1_h0_n0ab_f0cd", moduleConfig.detection_sensor.name);
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(existing.restricted_routing_mode, stored.restricted_routing_mode);
    powerSafe = true;
}

void test_transaction_commit_failure_can_be_retried()
{
    admin->deferSaves();
    TEST_ASSERT_TRUE(admin->handleSetModuleConfig(sensorConfig("_tr4")));
    powerSafe = false;
    commitEdit();
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
    TEST_ASSERT_NOT_NULL(admin->reply());
    admin->drainReply();
    tmesh_FeatureFlagsConfig stored = tmesh_FeatureFlagsConfig_init_zero;
    TEST_ASSERT_FALSE(FeatureFlags::copyConfig(stored));
    powerSafe = true;
    commitEdit();
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
    FeatureFlags::loadConfig(*nodeDB);
    TEST_ASSERT_TRUE(FeatureFlags::copyConfig(stored));
    TEST_ASSERT_EQUAL(tmesh_FeatureFlagsConfig_RestrictedRoutingMode_TEXT_ALL_NODEINFO_ADMIN_AND_ROUTING,
                      stored.restricted_routing_mode);
}
#endif

void setup()
{
    initializeTestEnvironment();
    service = new MockMeshService();
    nodeDB = new NodeDB();
    router = new Router();
    routingModule = new RoutingModule();
    channels.initDefaults();
    channels.onConfigChanged();
    UNITY_BEGIN();
    RUN_TEST(test_custom_config_overrides_legacy_and_survives_reload);
    RUN_TEST(test_invalid_config_does_not_change_active_values);
    RUN_TEST(test_empty_custom_config_does_not_inherit_legacy_flags);
    RUN_TEST(test_remote_authorized_admin_cannot_set_tmesh_flags);
    RUN_TEST(test_invalid_store_suppresses_legacy_and_reports_error);
    RUN_TEST(test_sensor_update_replaces_config_and_survives_reload);
    RUN_TEST(test_legacy_parser_ignores_invalid_tokens_and_last_valid_wins);
    RUN_TEST(test_ordinary_sensor_updates_leave_flags_untouched);
    RUN_TEST(test_transaction_defers_and_commits_last_matching_update);
    RUN_TEST(test_abandoned_transaction_persists_sensor_flags);
    RUN_TEST(test_direct_tmesh_write_supersedes_deferred_sensor_update);
    RUN_TEST(test_remote_sensor_update_requires_authorization_and_session);
#if !defined(_WIN32)
    RUN_TEST(test_storage_failure_does_not_apply_sensor_or_flags);
    RUN_TEST(test_transaction_commit_failure_can_be_retried);
#endif
    exit(UNITY_END());
}

void loop() {}
