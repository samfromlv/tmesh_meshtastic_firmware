#!/usr/bin/env python3
"""Read or replace TMesh feature flags over a local Meshtastic serial connection.

Requires: python -m pip install meshtastic grpcio-tools

Examples:
  python bin/tmesh-feature-flags.py --port /dev/ttyACM0 get
  python bin/tmesh-feature-flags.py --port /dev/ttyACM0 set --initialize \\
      --restricted-routing-mode TEXT_ALL_AND_ADMIN
  python bin/tmesh-feature-flags.py --port /dev/ttyACM0 set \\
      --force-next-hop-mode LOCAL_PACKETS_WITH_FLOODING_FALLBACK --force-next-hop-relay 0xab
  python bin/tmesh-feature-flags.py --port /dev/ttyACM0 clear

SET replaces the entire TMesh config. When one already exists, unspecified fields
are preserved by reading it first. --initialize explicitly starts with defaults
when no custom config exists; legacy detection-sensor flags are not migrated.
CLEAR restores the detection-sensor fallback. Both writes are read back.
"""

import argparse
import importlib.util
import json
import math
import queue
import sys
import tempfile
from pathlib import Path

PROTO = Path(__file__).resolve().parents[1] / "src" / "tmesh" / "feature_flags.proto"


def load_schema():
    try:
        from grpc_tools import protoc
    except ImportError as exc:
        raise RuntimeError(
            f"Cannot import grpcio-tools ({exc}). Install: python -m pip install meshtastic grpcio-tools"
        ) from exc

    with tempfile.TemporaryDirectory(prefix="tmesh-proto-") as directory:
        result = protoc.main(
            [
                "grpc_tools.protoc",
                f"-I{PROTO.parent}",
                f"--python_out={directory}",
                str(PROTO),
            ]
        )
        if result:
            raise RuntimeError(f"Could not compile {PROTO} (protoc exit {result})")
        spec = importlib.util.spec_from_file_location(
            "tmesh_feature_flags_pb2", Path(directory) / "feature_flags_pb2.py"
        )
        if spec is None or spec.loader is None:
            raise RuntimeError("Could not load generated TMesh protobuf")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


def request(interface, schema, operation, timeout, config=None):
    from google.protobuf.message import DecodeError
    from meshtastic.protobuf import portnums_pb2

    if not interface.myInfo or not interface.myInfo.my_node_num:
        raise RuntimeError(
            "Connected device did not provide its node number; refusing to send"
        )
    envelope = schema.Envelope()
    envelope.request.operation = operation
    if config is not None:
        envelope.request.config.CopyFrom(config)

    responses = queue.Queue()

    def on_response(packet):
        responses.put_nowait(packet)

    try:
        interface.sendData(
            envelope.SerializeToString(),
            destinationId=interface.myInfo.my_node_num,
            portNum=portnums_pb2.ADMIN_APP,
            wantResponse=True,
            onResponse=on_response,
            wantAck=False,
            hopLimit=0,
        )
    except (OSError, RuntimeError) as exc:
        uncertainty = (
            " The write may have been sent; read the config before retrying."
            if operation != schema.Request.GET
            else ""
        )
        raise RuntimeError(
            f"Failed to send TMesh request: {exc}.{uncertainty}"
        ) from exc
    try:
        packet = responses.get(timeout=timeout)
    except queue.Empty as exc:
        uncertainty = (
            " The write may already have been applied; read the config before retrying."
            if config is not None or operation == schema.Request.CLEAR
            else ""
        )
        raise RuntimeError(
            "Timed out waiting for the local TMesh response. Check firmware version and local admin access."
            + uncertainty
        ) from exc

    decoded = packet.get("decoded", {})
    if decoded.get("portnum") != "ADMIN_APP":
        raise RuntimeError(
            f"Device returned a non-admin response: {decoded.get('routing', decoded)}"
        )
    reply = schema.Envelope()
    try:
        reply.ParseFromString(decoded["payload"])
    except (KeyError, DecodeError) as exc:
        raise RuntimeError("Device returned a malformed TMesh response") from exc
    if reply.WhichOneof("payload") != "response":
        raise RuntimeError("Device did not return a TMesh response")
    response = reply.response
    if response.status != schema.Response.OK:
        try:
            status = schema.Response.Status.Name(response.status)
        except ValueError:
            status = str(response.status)
        raise RuntimeError(f"Device rejected TMesh request: {status}")
    return response


def config_dict(config):
    from google.protobuf.json_format import MessageToDict

    return MessageToDict(
        config,
        preserving_proto_field_name=True,
        always_print_fields_with_no_presence=True,
    )


def parse_byte(value):
    try:
        number = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "relay must be a decimal or 0x-prefixed byte"
        ) from exc
    if not 0 <= number <= 255:
        raise argparse.ArgumentTypeError("relay must be between 0 and 255")
    return number


def parser_for(schema):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--port", required=True, help="Local serial port, e.g. /dev/ttyACM0"
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=15,
        help="Seconds to wait for each response (default: 15)",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("get", help="Read custom configuration")
    commands.add_parser(
        "clear", help="Remove custom configuration and restore legacy fallback"
    )
    update = commands.add_parser("set", help="Update and verify custom configuration")
    update.add_argument(
        "--initialize",
        action="store_true",
        help="Start with defaults if no custom config exists",
    )
    for field in (
        "restricted_routing_mode",
        "hop_decrement_mode",
        "force_next_hop_mode",
        "favorite_router_mode",
    ):
        enum = schema.FeatureFlagsConfig.DESCRIPTOR.fields_by_name[field].enum_type
        update.add_argument(
            f"--{field.replace('_', '-')}", choices=[v.name for v in enum.values]
        )
    update.add_argument("--force-next-hop-relay", type=parse_byte)
    update.add_argument("--favorite-router-relay", type=parse_byte)
    private = update.add_mutually_exclusive_group()
    private.add_argument(
        "--allow-private-text", dest="disallow_private", action="store_false"
    )
    private.add_argument(
        "--disallow-private-text", dest="disallow_private", action="store_true"
    )
    update.set_defaults(disallow_private=None)
    return parser


def execute(interface, schema, args):
    current = request(interface, schema, schema.Request.GET, args.timeout)
    if args.command == "get":
        return {
            "source": "custom" if current.HasField("config") else "detection_sensor",
            "config": (
                config_dict(current.config) if current.HasField("config") else None
            ),
        }

    if args.command == "clear":
        request(interface, schema, schema.Request.CLEAR, args.timeout)
        try:
            checked = request(interface, schema, schema.Request.GET, args.timeout)
        except RuntimeError as exc:
            raise RuntimeError(
                f"CLEAR was acknowledged but read-back failed: {exc}"
            ) from exc
        if checked.HasField("config"):
            raise RuntimeError("CLEAR read-back failed: custom config still exists")
        return {"source": "detection_sensor", "config": None, "verified": True}

    fields = (
        "restricted_routing_mode",
        "hop_decrement_mode",
        "force_next_hop_mode",
        "favorite_router_mode",
        "force_next_hop_relay",
        "favorite_router_relay",
    )
    changes = {
        field: getattr(args, field)
        for field in fields
        if getattr(args, field) is not None
    }
    if args.disallow_private is not None:
        changes["restricted_routing_disallows_private_text"] = args.disallow_private
    if not changes:
        raise RuntimeError("SET requires at least one flag to change")
    config = schema.FeatureFlagsConfig()
    if current.HasField("config"):
        config.CopyFrom(current.config)
    elif not args.initialize:
        raise RuntimeError(
            "No custom config exists. Use --initialize to replace legacy flags with defaults and the supplied values."
        )
    for name, value in changes.items():
        field = config.DESCRIPTOR.fields_by_name[name]
        setattr(
            config,
            name,
            field.enum_type.values_by_name[value].number if field.enum_type else value,
        )
    if config.force_next_hop_mode and not config.force_next_hop_relay:
        raise RuntimeError(
            "An enabled force-next-hop mode requires a nonzero relay byte"
        )
    if config.favorite_router_mode and not config.favorite_router_relay:
        raise RuntimeError(
            "An enabled favorite-router mode requires a nonzero relay byte"
        )
    request(interface, schema, schema.Request.SET, args.timeout, config)
    try:
        checked = request(interface, schema, schema.Request.GET, args.timeout)
    except RuntimeError as exc:
        raise RuntimeError(f"SET was acknowledged but read-back failed: {exc}") from exc
    if not checked.HasField("config") or checked.config != config:
        raise RuntimeError(
            "SET read-back failed: device configuration differs from the requested values"
        )
    return {"source": "custom", "config": config_dict(checked.config), "verified": True}


def main():
    try:
        schema = load_schema()
        args = parser_for(schema).parse_args()
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise RuntimeError("--timeout must be a positive finite number")
        try:
            from meshtastic.serial_interface import SerialInterface
        except ImportError as exc:
            raise RuntimeError(
                f"Cannot import Meshtastic ({exc}). Install: python -m pip install meshtastic grpcio-tools"
            ) from exc
        interface = SerialInterface(devPath=args.port)
        try:
            result = execute(interface, schema, args)
        finally:
            interface.close()
        print(json.dumps(result, indent=2))
        return 0
    except (RuntimeError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
