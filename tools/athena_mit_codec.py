#!/usr/bin/env python3
"""MIT five-parameter codec used by the normal GD32 application.

This module is deliberately transport-free: it only encodes/decodes frames.
Keeping it separate from UC12/SLCAN avoids confusing USB TX acknowledgements
with CAN feedback and makes the third-party MIT examples testable offline.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import os


@dataclass(frozen=True)
class MitRanges:
    position_min: float = -12.5
    position_max: float = 12.5
    velocity_min: float = -65.0
    velocity_max: float = 65.0
    kp_max: float = 500.0
    kd_max: float = 5.0
    torque_max: float = 40.0


def _position_range_from_environment() -> MitRanges:
    try:
        minimum = float(os.environ.get("ATHENA_MIT_POSITION_MIN", "-12.5"))
        maximum = float(os.environ.get("ATHENA_MIT_POSITION_MAX", "12.5"))
    except ValueError:
        return MitRanges()
    if not math.isfinite(minimum) or not math.isfinite(maximum) or minimum >= 0.0 or maximum <= 0.0:
        return MitRanges()
    return MitRanges(position_min=minimum, position_max=maximum)


DEFAULT_RANGES = _position_range_from_environment()


def _quantize(value: float, minimum: float, maximum: float, bits: int) -> int:
    if not math.isfinite(value):
        raise ValueError("MIT parameter must be finite")
    if minimum >= maximum:
        raise ValueError("invalid MIT parameter range")
    value = min(maximum, max(minimum, value))
    return int((value - minimum) * ((1 << bits) - 1) / (maximum - minimum))


def _dequantize(value: int, minimum: float, maximum: float, bits: int) -> float:
    return value * (maximum - minimum) / ((1 << bits) - 1) + minimum


def encode_command(position: float, velocity: float, kp: float, kd: float,
                   torque: float, ranges: MitRanges = DEFAULT_RANGES) -> bytes:
    """Encode one normal-app MIT command into exactly eight data bytes."""
    p = _quantize(position, ranges.position_min, ranges.position_max, 16)
    v = _quantize(velocity, ranges.velocity_min, ranges.velocity_max, 12)
    k_p = _quantize(kp, 0.0, ranges.kp_max, 12)
    k_d = _quantize(kd, 0.0, ranges.kd_max, 12)
    t = _quantize(torque, -ranges.torque_max, ranges.torque_max, 12)
    return bytes((p >> 8, p & 0xff, v >> 4, ((v & 0xf) << 4) | (k_p >> 8),
                  k_p & 0xff, k_d >> 4, ((k_d & 0xf) << 4) | (t >> 8),
                  t & 0xff))


def decode_feedback(data: bytes, ranges: MitRanges = DEFAULT_RANGES) -> dict[str, float | int]:
    """Decode the six-byte feedback payload emitted by the normal app.

    Byte 0 is the controller CAN ID; bytes 1..5 contain position, velocity,
    and torque. The current firmware sends DLC=6 and leaves bytes 6..7 clear.
    """
    if len(data) != 6:
        raise ValueError("normal feedback must contain exactly 6 data bytes")
    p = (data[1] << 8) | data[2]
    v = (data[3] << 4) | (data[4] >> 4)
    t = ((data[4] & 0xf) << 8) | data[5]
    return {"id": data[0],
            "position": _dequantize(p, ranges.position_min, ranges.position_max, 16),
            "velocity": _dequantize(v, ranges.velocity_min, ranges.velocity_max, 12),
            "torque": _dequantize(t, -ranges.torque_max, ranges.torque_max, 12)}


def special_command(code: int) -> bytes:
    """Return the explicit command frame (0xFC/FD/FE) used by the firmware."""
    if code not in (0xFC, 0xFD, 0xFE):
        raise ValueError("only firmware-defined special commands are allowed")
    return b"\xff\xff\xff\xff\xff\xff\xff" + bytes((code,))


def format_slcan(can_id: int, data: bytes) -> str:
    if not 0 <= can_id <= 0x7ff:
        raise ValueError("only standard CAN IDs are supported")
    if not 0 <= len(data) <= 8:
        raise ValueError("classic CAN data length must be 0..8")
    return f"t{can_id:03X}{len(data):X}{data.hex().upper()}\r"


def parse_slcan(frame: str) -> tuple[int, bytes]:
    text = frame.strip()
    if len(text) < 5 or text[0] != "t":
        raise ValueError("only standard SLCAN data frames are accepted")
    try:
        can_id = int(text[1:4], 16)
        dlc = int(text[4], 16)
    except ValueError as exc:
        raise ValueError("invalid SLCAN header") from exc
    payload = text[5:]
    if dlc > 8 or len(payload) != dlc * 2:
        raise ValueError("SLCAN DLC does not match payload length")
    try:
        data = bytes.fromhex(payload)
    except ValueError as exc:
        raise ValueError("invalid SLCAN payload") from exc
    return can_id, data


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="offline MIT/SLCAN codec")
    sub = parser.add_subparsers(dest="action", required=True)
    enc = sub.add_parser("encode")
    enc.add_argument("position", type=float)
    enc.add_argument("velocity", type=float)
    enc.add_argument("kp", type=float)
    enc.add_argument("kd", type=float)
    enc.add_argument("torque", type=float)
    enc.add_argument("--id", type=lambda value: int(value, 0), default=1)
    dec = sub.add_parser("decode")
    dec.add_argument("frame")
    args = parser.parse_args()
    if args.action == "encode":
        print(format_slcan(args.id, encode_command(args.position, args.velocity,
                                                   args.kp, args.kd, args.torque)))
    else:
        can_id, payload = parse_slcan(args.frame)
        if can_id != 0 or len(payload) != 6:
            raise SystemExit("expected standard ID 0x000 with six-byte feedback")
        print(decode_feedback(payload))
