#!/usr/bin/env python3
"""Tests for bin/tmesh-feature-flags.py's local admin protocol.

The client must preserve unspecified fields on full-config writes, refuse
implicit migration from legacy flags, and verify persisted results. These
tests guard against data loss and falsely reporting a failed write as success.
The receive-path test also checks the official Meshtastic ADMIN_APP decoder
does not hide the custom payload before the response callback runs.
"""

import importlib.util
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import admin_pb2, mesh_pb2, portnums_pb2

spec = importlib.util.spec_from_file_location(
    "tmesh_client", Path(__file__).with_name("tmesh-feature-flags.py")
)
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
schema = client.load_schema()


class FakeDevice:
    def __init__(self, config=None):
        self.myInfo = SimpleNamespace(my_node_num=0x1234)
        self.config = config
        self.calls = []
        self.responses = []
        self.discard_writes = False

    def sendData(self, payload, **kwargs):
        message = schema.Envelope.FromString(payload)
        self.calls.append((message.request, kwargs))
        if self.responses:
            packet = self.responses.pop(0)
            if packet is not None:
                kwargs["onResponse"](packet)
            return
        response = schema.Envelope()
        response.response.status = schema.Response.OK
        if message.request.operation == schema.Request.SET and not self.discard_writes:
            self.config = schema.FeatureFlagsConfig()
            self.config.CopyFrom(message.request.config)
        elif (
            message.request.operation == schema.Request.CLEAR
            and not self.discard_writes
        ):
            self.config = None
        if self.config is not None:
            response.response.config.CopyFrom(self.config)
        kwargs["onResponse"](
            {
                "decoded": {
                    "portnum": "ADMIN_APP",
                    "payload": response.SerializeToString(),
                }
            }
        )


class ClientTests(unittest.TestCase):
    def args(self, *args):
        return client.parser_for(schema).parse_args(
            ["--port", "fake", "--timeout", "0.001", *args]
        )

    def test_get_fallback_and_local_address(self):
        device = FakeDevice()
        self.assertEqual(
            client.execute(device, schema, self.args("get")),
            {"source": "detection_sensor", "config": None},
        )
        request, kwargs = device.calls[0]
        self.assertEqual(request.operation, schema.Request.GET)
        self.assertEqual(kwargs["destinationId"], device.myInfo.my_node_num)
        self.assertEqual(kwargs["portNum"], portnums_pb2.ADMIN_APP)
        self.assertTrue(kwargs["wantResponse"])
        self.assertEqual(kwargs["hopLimit"], 0)

    def test_set_requires_explicit_initialization(self):
        device = FakeDevice()
        with self.assertRaisesRegex(RuntimeError, "--initialize"):
            client.execute(device, schema, self.args("set", "--disallow-private-text"))
        self.assertEqual(len(device.calls), 1)

    def test_set_initializes_and_verifies(self):
        device = FakeDevice()
        result = client.execute(
            device, schema, self.args("set", "--initialize", "--disallow-private-text")
        )
        self.assertTrue(result["verified"])
        self.assertTrue(device.config.restricted_routing_disallows_private_text)
        self.assertEqual(
            [call[0].operation for call in device.calls],
            [schema.Request.GET, schema.Request.SET, schema.Request.GET],
        )

    def test_set_preserves_other_fields(self):
        old = schema.FeatureFlagsConfig(
            hop_decrement_mode=2, force_next_hop_mode=1, force_next_hop_relay=0xAB
        )
        device = FakeDevice(old)
        result = client.execute(
            device,
            schema,
            self.args("set", "--restricted-routing-mode", "TEXT_ALL_AND_ADMIN"),
        )
        self.assertTrue(result["verified"])
        self.assertEqual(device.config.hop_decrement_mode, 2)
        self.assertEqual(device.config.force_next_hop_relay, 0xAB)
        self.assertEqual(device.config.restricted_routing_mode, 1)

    def test_set_relay_byte_boundaries(self):
        device = FakeDevice()
        client.execute(
            device,
            schema,
            self.args(
                "set",
                "--initialize",
                "--force-next-hop-mode",
                "LOCAL_PACKETS_WITH_FLOODING_FALLBACK",
                "--force-next-hop-relay",
                "0xff",
            ),
        )
        self.assertEqual(device.config.force_next_hop_relay, 255)
        with self.assertRaisesRegex(RuntimeError, "nonzero relay"):
            client.execute(
                device, schema, self.args("set", "--force-next-hop-relay", "0")
            )

    def test_validation_prevents_invalid_write(self):
        device = FakeDevice()
        with self.assertRaisesRegex(RuntimeError, "nonzero relay"):
            client.execute(
                device,
                schema,
                self.args(
                    "set",
                    "--initialize",
                    "--force-next-hop-mode",
                    "LOCAL_PACKETS_WITH_FLOODING_FALLBACK",
                ),
            )
        self.assertEqual(len(device.calls), 1)
        with self.assertRaises(SystemExit):
            self.args("set", "--favorite-router-relay", "256")
        with self.assertRaises(SystemExit):
            self.args("set", "--favorite-router-mode", "unknown")

    def test_clear_and_mismatch_detection(self):
        device = FakeDevice(schema.FeatureFlagsConfig(hop_decrement_mode=1))
        self.assertTrue(client.execute(device, schema, self.args("clear"))["verified"])
        self.assertIsNone(device.config)
        device.discard_writes = True
        with self.assertRaisesRegex(RuntimeError, "read-back failed"):
            client.execute(
                device,
                schema,
                self.args("set", "--initialize", "--disallow-private-text"),
            )
        device.config = schema.FeatureFlagsConfig()
        with self.assertRaisesRegex(RuntimeError, "read-back failed"):
            client.execute(device, schema, self.args("clear"))

    def test_timeouts_and_routing_error(self):
        device = FakeDevice()
        device.responses = [None]
        with self.assertRaisesRegex(RuntimeError, "Timed out"):
            client.request(device, schema, schema.Request.GET, 0.001)
        device.responses = [None]
        with self.assertRaisesRegex(RuntimeError, "may already have been applied"):
            client.request(
                device, schema, schema.Request.SET, 0.001, schema.FeatureFlagsConfig()
            )
        device.responses = [None]
        with self.assertRaisesRegex(RuntimeError, "may already have been applied"):
            client.request(device, schema, schema.Request.CLEAR, 0.001)
        device.responses = [
            {
                "decoded": {
                    "portnum": "ROUTING_APP",
                    "routing": {"errorReason": "NOT_AUTHORIZED"},
                }
            }
        ]
        with self.assertRaisesRegex(RuntimeError, "NOT_AUTHORIZED"):
            client.request(device, schema, schema.Request.GET, 0.001)

    def test_response_errors_and_missing_node(self):
        device = FakeDevice()
        response = schema.Envelope()
        response.response.status = schema.Response.STORAGE_ERROR
        device.responses = [
            {
                "decoded": {
                    "portnum": "ADMIN_APP",
                    "payload": response.SerializeToString(),
                }
            }
        ]
        with self.assertRaisesRegex(RuntimeError, "STORAGE_ERROR"):
            client.request(device, schema, schema.Request.GET, 0.001)
        device.responses = [{"decoded": {"portnum": "ADMIN_APP", "payload": b"\x80"}}]
        with self.assertRaisesRegex(RuntimeError, "malformed"):
            client.request(device, schema, schema.Request.GET, 0.001)
        device.myInfo = None
        with self.assertRaisesRegex(RuntimeError, "node number"):
            client.request(device, schema, schema.Request.GET, 0.001)

    def test_official_admin_decoder_preserves_custom_payload_for_callback(self):
        reply = schema.Envelope()
        reply.response.status = schema.Response.OK
        reply.response.config.hop_decrement_mode = 1
        payload = reply.SerializeToString()
        official = admin_pb2.AdminMessage.FromString(payload)
        self.assertEqual(
            schema.Envelope.FromString(official.SerializeToString()), reply
        )

        interface = MeshInterface()
        interface.nodesByNum = {}
        received = []
        interface._addResponseHandler(42, received.append)
        packet = mesh_pb2.MeshPacket()
        setattr(packet, "from", 0x1234)
        packet.decoded.portnum = portnums_pb2.ADMIN_APP
        packet.decoded.request_id = 42
        packet.decoded.payload = payload
        with patch("meshtastic.mesh_interface.publishingThread.queueWork"):
            interface._handlePacketFromRadio(packet)
        self.assertEqual(received[0]["decoded"]["payload"], payload)
        self.assertEqual(
            schema.Envelope.FromString(received[0]["decoded"]["payload"]), reply
        )


if __name__ == "__main__":
    unittest.main()
