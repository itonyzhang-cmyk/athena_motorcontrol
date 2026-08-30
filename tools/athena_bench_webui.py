#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Small authenticated LAN panel for the reviewed Athena bench workflow.

Only named workflow actions below can execute. There is deliberately no shell
command input endpoint.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import secrets
import shlex
import socket
import subprocess
import threading
import time
import re
import select
import signal
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from athena_mit_codec import (
    DEFAULT_RANGES,
    MitRanges,
    decode_feedback,
    encode_command,
    feedback_position_delta,
    format_slcan,
)


REPO = Path(__file__).resolve().parent.parent
WORKSPACE = REPO.parent
# Keep the compiler outside /tmp so a cleanup cannot silently remove the
# runtime used by the WebUI build action.  The legacy temporary path remains a
# fallback for existing bench setups.
_TOOLCHAIN_CANDIDATES = (
    Path(os.environ.get(
        "ATHENA_TOOLCHAIN",
        str(Path.home() / ".cache/arm-gnu-toolchain-15.2.rel1-20260825/bin"))),
    Path("/Users/choqy/toolchains/arm-gnu-toolchain-15.2/bin"),
    Path("/Users/choqy/toolchains/arm-gnu-toolchain-15.2/bin"),
)
TOOLCHAIN = next((path for path in _TOOLCHAIN_CANDIDATES
                  if (path / "arm-none-eabi-gcc").is_file()),
                 _TOOLCHAIN_CANDIDATES[0])
NORMAL_CONFIG = REPO / "athena_bench_webui.json"
BRIDGE = WORKSPACE / "tools/uc12_slcan_bridge/uc12_slcan_bridge"
DIAG = REPO / "tools/athena_diag_uc12/athena_diag_uc12"
FLASH = REPO / "tools/athena_safe_flash.sh"

ENABLE_FRAME = "t0018FFFFFFFFFFFFFFFC\r"
STOP_FRAME = "t0018FFFFFFFFFFFFFFFD\r"
# USB-CAN/PTY scheduling can occasionally stall for several milliseconds. Keep
# a 5 ms nominal period and schedule against absolute deadlines so a slow write
# does not add delay to every subsequent frame (the firmware watchdog is ~33 ms).
try:
    MIT_KEEPALIVE_INTERVAL_S = float(os.environ.get("ATHENA_MIT_INTERVAL_S", "0.005"))
except ValueError:
    MIT_KEEPALIVE_INTERVAL_S = 0.005
if not 0.002 <= MIT_KEEPALIVE_INTERVAL_S <= 0.1:
    MIT_KEEPALIVE_INTERVAL_S = 0.005
PTY_WRITE_TIMEOUT_S = 0.25
MIT_DEFAULT_DURATION_S = 10.0
MIT_MAX_DURATION_S = 300.0
TRAJECTORY_MAX_SPEED_RAD_S = 20.0
FEEDBACK_RE = re.compile(r"TRACE CAN RX t000#([0-9A-Fa-f]{12})")
TX_RE = re.compile(r"TRACE CAN TX t001#([0-9A-Fa-f]{16})")
QUIET_TRACE = os.environ.get("ATHENA_QUIET_TRACE", "0") == "1"


def normal_firmware_config() -> tuple[str, Path]:
    """Read the selected normal image on each request so config changes are live."""
    try:
        data = json.loads(NORMAL_CONFIG.read_text(encoding="utf-8"))
        sha = str(data["normal_sha"]).strip().lower()
        image = (REPO / str(data["normal_image"])).resolve()
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"正常固件配置无效: {exc}") from exc
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        raise RuntimeError("正常固件配置中的 SHA-256 无效")
    if REPO not in image.parents or not image.is_file():
        raise RuntimeError("正常固件镜像路径无效")
    return sha, image


def configured_mit_protocol() -> tuple[MitRanges, float, float]:
    """Load the normal-firmware MIT contract, always in motor-side units.

    Firmware owns this contract: P/V/Kp/Kd come from its persisted registers
    and torque is exactly I_MAX * KT.  The upper controller must never infer
    one of these fields from a reducer ratio or a generic MIT default.
    """
    try:
        data = json.loads(NORMAL_CONFIG.read_text(encoding="utf-8"))
        protocol = data["firmware_mit"]
        current_limit = float(protocol["current_limit_a"])
        torque_constant = float(protocol["torque_constant_nm_per_a"])
        ranges = MitRanges(
            position_min=float(protocol["position_min"]),
            position_max=float(protocol["position_max"]),
            velocity_min=float(protocol["velocity_min"]),
            velocity_max=float(protocol["velocity_max"]),
            kp_max=float(protocol["kp_max"]),
            kd_max=float(protocol["kd_max"]),
            torque_max=current_limit * torque_constant)
    except (OSError, ValueError, KeyError, TypeError):
        raise RuntimeError("正常固件 MIT 协议清单无效")
    values = (ranges.position_min, ranges.position_max, ranges.velocity_min,
              ranges.velocity_max, ranges.kp_max, ranges.kd_max,
              current_limit, torque_constant)
    if (not all(math.isfinite(value) for value in values) or
            not ranges.position_min < ranges.position_max or
            not ranges.velocity_min < ranges.velocity_max or
            ranges.kp_max < 0.0 or ranges.kd_max < 0.0 or
            current_limit <= 0.0 or torque_constant <= 0.0):
        raise RuntimeError("正常固件 MIT 协议清单范围无效")
    return ranges, current_limit, torque_constant


def configured_output_reduction() -> float:
    """Load the motor-turns per output-turn ratio used by the upper controller."""
    try:
        reduction = float(json.loads(NORMAL_CONFIG.read_text(encoding="utf-8"))["output_reduction"])
    except (OSError, ValueError, KeyError, TypeError):
        return 1.0
    return reduction if math.isfinite(reduction) and reduction >= 1.0 else 1.0


def persist_output_reduction(reduction: float) -> None:
    """Persist an upper-controller-only kinematic mapping without touching CAN."""
    data = json.loads(NORMAL_CONFIG.read_text(encoding="utf-8"))
    data["output_reduction"] = reduction
    temporary = NORMAL_CONFIG.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(NORMAL_CONFIG)


def output_to_motor(position: float, velocity: float, kp: float, kd: float,
                    torque: float, reduction: float) -> tuple[float, float, float, float, float]:
    """Map only output-axis kinematics; MIT gains and torque stay motor-side."""
    return (position * reduction, velocity * reduction, kp, kd, torque)


def compose_feedforward_torque(gravity_torque: float, friction_torque: float,
                               velocity: float, position_error: float) -> float:
    """Compose signed MIT feed-forward from upper-controller load terms.

    Gravity is already signed by the operator/model. Physical Coulomb friction
    opposes the intended motion, so its *compensation command* has the same
    sign as the intended motion. At zero velocity, use the position-error
    direction so a position step can overcome static friction without manual
    sign changes.
    """
    direction = velocity if abs(velocity) > 1e-6 else position_error
    friction = math.copysign(abs(friction_torque), direction) if abs(direction) > 1e-6 else 0.0
    return gravity_torque + friction


def updated_firmware_mit_protocol(ranges: MitRanges, current_limit: float,
                                  torque_constant: float, field: int,
                                  value: float) -> tuple[MitRanges, float, float]:
    """Apply one accepted firmware configuration field to the host contract."""
    values = {
        "position_min": ranges.position_min, "position_max": ranges.position_max,
        "velocity_min": ranges.velocity_min, "velocity_max": ranges.velocity_max,
        "kp_max": ranges.kp_max, "kd_max": ranges.kd_max,
    }
    field_names = {14: "position_min", 15: "position_max", 16: "velocity_min",
                   17: "velocity_max", 18: "kp_max", 19: "kd_max"}
    if field in field_names:
        values[field_names[field]] = float(value)
    elif field == 7:
        current_limit = float(value)
    elif field == 12:
        torque_constant = float(value)
    else:
        return ranges, current_limit, torque_constant
    candidate = MitRanges(**values, torque_max=current_limit * torque_constant)
    valid = (math.isfinite(current_limit) and math.isfinite(torque_constant) and
             0.1 <= current_limit <= 60.0 and 0.0001 <= torque_constant <= 10.0 and
             all(math.isfinite(item) for item in (
                 candidate.position_min, candidate.position_max,
                 candidate.velocity_min, candidate.velocity_max,
                 candidate.kp_max, candidate.kd_max)) and
             -1000.0 <= candidate.position_min <= 0.0 and
             0.0 <= candidate.position_max <= 1000.0 and
             candidate.position_min < candidate.position_max and
             -1000.0 <= candidate.velocity_min <= 0.0 and
             0.0 <= candidate.velocity_max <= 1000.0 and
             candidate.velocity_min < candidate.velocity_max and
             0.0 <= candidate.kp_max <= 1000.0 and
             0.0 <= candidate.kd_max <= 100.0)
    if not valid:
        raise ValueError("MIT 协议范围无效")
    return candidate, current_limit, torque_constant


def persist_mit_protocol(ranges: MitRanges, current_limit: float,
                         torque_constant: float) -> None:
    """Persist only a firmware-confirmed MIT configuration transaction."""
    data = json.loads(NORMAL_CONFIG.read_text(encoding="utf-8"))
    data["firmware_mit"] = {
        "position_min": ranges.position_min,
        "position_max": ranges.position_max,
        "velocity_min": ranges.velocity_min,
        "velocity_max": ranges.velocity_max,
        "kp_max": ranges.kp_max,
        "kd_max": ranges.kd_max,
        "current_limit_a": current_limit,
        "torque_constant_nm_per_a": torque_constant,
    }
    temporary = NORMAL_CONFIG.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(NORMAL_CONFIG)

def _semantic_can_line(clean: str, ranges: MitRanges) -> str | None:
    tx = TX_RE.search(clean)
    if tx:
        data = bytes.fromhex(tx.group(1))
        if data == b"\xff" * 7 + b"\xfc":
            return "CAN 语义 TX: 使能请求（0xFC）"
        if data == b"\xff" * 7 + b"\xfd":
            return "CAN 语义 TX: 停止请求（0xFD）"
        if len(data) == 8:
            p = (data[0] << 8) | data[1]
            v = (data[2] << 4) | (data[3] >> 4)
            kp = ((data[3] & 0xF) << 8) | data[4]
            kd = (data[5] << 4) | (data[6] >> 4)
            tq = ((data[6] & 0xF) << 8) | data[7]
            torque = tq * (2.0 * ranges.torque_max) / 4095.0 - ranges.torque_max
            if abs(torque) <= 0.01:
                torque = 0.0
            return ("CAN 语义 TX: MIT(电机侧) p={:.4f} rad, v={:.3f} rad/s, "
                    "Kp={:.2f}, Kd={:.3f}, t_ff={:.3f} Nm".format(
                        p * (ranges.position_max - ranges.position_min) / 65535.0 + ranges.position_min,
                        v * (ranges.velocity_max - ranges.velocity_min) / 4095.0 + ranges.velocity_min,
                        kp * ranges.kp_max / 4095.0,
                        kd * ranges.kd_max / 4095.0,
                        torque))
    rx = FEEDBACK_RE.search(clean)
    if rx:
        decoded = decode_feedback(bytes.fromhex(rx.group(1)), ranges)
        torque = float(decoded["torque"])
        if abs(torque) <= 0.01:
            torque = 0.0
        decoded["torque"] = torque
        return ("CAN 语义 RX: 电机侧反馈(上一控制周期) p={position:.4f} rad, "
                "v_est={velocity:.3f} rad/s, t_filt={torque:.3f} Nm"
                .format(**decoded))
    return None


def _mit_command_details(payload: bytes, ranges: MitRanges) -> str:
    """Render the exact MIT command payload using the live firmware ranges."""
    if len(payload) != 8:
        raise ValueError("MIT command payload must contain exactly 8 bytes")
    p = (payload[0] << 8) | payload[1]
    v = (payload[2] << 4) | (payload[3] >> 4)
    kp = ((payload[3] & 0x0F) << 8) | payload[4]
    kd = (payload[5] << 4) | (payload[6] >> 4)
    tq = ((payload[6] & 0x0F) << 8) | payload[7]
    # Position is the only 16-bit field; the remaining MIT fields are 12-bit.
    position = p * (ranges.position_max - ranges.position_min) / 65535.0 + ranges.position_min
    velocity = v * (ranges.velocity_max - ranges.velocity_min) / 4095.0 + ranges.velocity_min
    kp_value = kp * ranges.kp_max / 4095.0
    kd_value = kd * ranges.kd_max / 4095.0
    torque = tq * (2.0 * ranges.torque_max) / 4095.0 - ranges.torque_max
    if abs(torque) <= 0.01:
        torque = 0.0
    return (f"SLCAN=t0018{payload.hex().upper()} payload={payload.hex().upper()} "
            f"解析 p={position:.4f} rad, v={velocity:.3f} rad/s, "
            f"Kp={kp_value:.3f}, Kd={kd_value:.3f}, t_ff={torque:+.3f} Nm")


def _diag_crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xff if crc & 0x80 else (crc << 1) & 0xff
    return crc


class Runner:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.mit_ranges, self.mit_current_limit, self.mit_torque_constant = configured_mit_protocol()
        self.output_reduction = configured_output_reduction()
        self.active: dict[str, Any] | None = None
        self.logs: deque[str] = deque(maxlen=1600)
        self.last_result: dict[str, Any] = {"state": "idle", "exit_code": None}
        self.bridge: subprocess.Popen[str] | None = None
        self.bridge_tty = ""
        self.mit_check_active = False
        self.enable_active = False
        self.mit_stop_event = threading.Event()
        self.mit_thread: threading.Thread | None = None
        self.mit_session_frames = 0
        self.last_feedback_position = ""
        self.last_feedback_position_rad: float | None = None
        self.last_feedback_velocity_rad_s: float | None = None
        self.last_feedback_torque_nm: float | None = None
        self.logical_position_rad: float | None = None
        self._logical_feedback_position_rad: float | None = None
        self._logical_tracking_active = False
        self.feedback_generation = 0
        self.diag_sequence = 0
        # Structured motion evidence survives noisy raw SLCAN trace output.
        # It records the host's exact MIT fields before they reach the bridge.
        self.last_motion_evidence: dict[str, Any] | None = None

    def log(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        for line in text.rstrip("\n").splitlines() or [""]:
            self.logs.append(f"[{stamp}] {line}")

    def _begin_motion_evidence(self, source: str, *, reduction: float,
                               output_target: float | None,
                               motor_target: float | None, kp: float,
                               kd: float, torque: float) -> None:
        with self.lock:
            self.last_motion_evidence = {
                "source": source,
                "started_monotonic_s": time.monotonic(),
                "reduction": reduction,
                "output_target_rad": output_target,
                "motor_target_rad": motor_target,
                "requested_kp": kp,
                "requested_kd": kd,
                "requested_torque_nm": torque,
                "frames": 0,
                "last_motor_position_rad": None,
                "last_motor_velocity_rad_s": None,
                "last_slcan_payload_hex": None,
                "feedback_samples": 0,
                "feedback_motor_position_min_rad": None,
                "feedback_motor_position_max_rad": None,
                "feedback_torque_peak_nm": 0.0,
                "finished": False,
            }

    def _record_motion_frame(self, position: float, velocity: float, kp: float,
                             kd: float, torque: float, payload: bytes) -> None:
        log_frame = False
        frame_index = 0
        with self.lock:
            ranges = self.mit_ranges
        with self.lock:
            evidence = self.last_motion_evidence
            if evidence is None:
                return
            evidence["frames"] += 1
            frame_index = evidence["frames"]
            log_frame = frame_index == 1
            evidence["last_motor_position_rad"] = position
            evidence["last_motor_velocity_rad_s"] = velocity
            evidence["last_kp"] = kp
            evidence["last_kd"] = kd
            evidence["last_torque_nm"] = torque
            evidence["last_slcan_payload_hex"] = payload.hex().upper()
        if log_frame:
            self.log("MIT 实际发送首帧: " + _mit_command_details(payload, ranges))

    def _finish_motion_evidence(self, max_gap_s: float) -> None:
        with self.lock:
            if self.last_motion_evidence is not None:
                self.last_motion_evidence["finished"] = True
                self.last_motion_evidence["max_frame_gap_ms"] = max_gap_s * 1000.0

    class _SerialWriter:
        """Non-blocking PTY writer so a wedged bridge cannot pin an action thread."""
        def __init__(self, path: str) -> None:
            self.fd = os.open(path, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)

        def __enter__(self) -> "Runner._SerialWriter":
            return self

        def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
            os.close(self.fd)

        def write(self, text: str) -> None:
            data = text.encode("ascii")
            view = memoryview(data)
            deadline = time.monotonic() + PTY_WRITE_TIMEOUT_S
            while view:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("PTY 写入超时；桥接可能已停止响应")
                _, writable, _ = select.select([], [self.fd], [], remaining)
                if not writable:
                    raise TimeoutError("PTY 写入超时；桥接可能已停止响应")
                try:
                    count = os.write(self.fd, view)
                except BlockingIOError:
                    continue
                if count <= 0:
                    raise OSError("PTY 写入返回 0")
                view = view[count:]

    def _open_serial(self, tty: str) -> "Runner._SerialWriter":
        return self._SerialWriter(tty)

    def clear_logs(self) -> None:
        with self.lock:
            self.logs.clear()

    def set_output_reduction(self, value: Any) -> tuple[bool, str]:
        """Update only the next-session output-to-motor kinematic conversion."""
        try:
            reduction = float(value)
        except (TypeError, ValueError):
            return False, "减速比必须是数字"
        if not math.isfinite(reduction) or not 1.0 <= reduction <= 1000.0:
            return False, "减速比必须在 1..1000 之间（电机转数/输出转数）"
        with self.lock:
            if self.enable_active or self.mit_check_active:
                return False, "CAN/MIT 动作运行中，停止后才能修改减速比"
            try:
                persist_output_reduction(reduction)
            except (OSError, ValueError, TypeError) as exc:
                return False, f"减速比未能保存: {exc}"
            self.output_reduction = reduction
        self.log(f"上位机减速比已设为 {reduction:g}:1；未发送 CAN 配置或控制帧")
        return True, f"减速比已保存为 {reduction:g}:1，仅影响后续轨迹的 P/V 映射"

    def send_diag(self, opcode: int, page: int, argument: int = 0) -> tuple[bool, str]:
        """Submit a diagnostic CAN frame through the already-running bridge."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            sequence = self.diag_sequence & 0xff
            self.diag_sequence = (self.diag_sequence + 1) & 0xff
        if not live or not tty:
            return False, "请先启动 CAN0 Trace；诊断请求必须复用桥接连接"
        if not 0 <= page <= 255 or not 0 <= argument <= 255:
            return False, "诊断 page/argument 超出范围"
        frame = bytearray((0xA5, 0x5A, 1, opcode & 0xff, sequence, page, argument, 0))
        frame[7] = _diag_crc8(frame[:7])
        slcan = "t7018" + frame.hex().upper() + "\r"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(slcan)
        except (OSError, TimeoutError) as exc:
            return False, f"诊断帧发送失败: {exc}"
        self.log(f"桥接诊断请求: opcode=0x{opcode:02X} page={page} argument={argument}")
        return True, "诊断请求已交给 CAN0 桥接"

    def send_debug_log(self, index: int) -> tuple[bool, str]:
        if not 0 <= index <= 31:
            return False, "日志索引必须是 0..31"
        for page in (10, 11, 12):
            ok, message = self.send_diag(0x07, page, index)
            if not ok:
                return False, message
            time.sleep(0.025)
        return True, f"已通过桥接请求 RAM 调试日志 #{index} 的时间戳、事件和 payload"

    def send_config_set(self, field: int, value: float | int, commit: bool) -> tuple[bool, str]:
        fields = ((0, 1), (0, 1), (0, 1), (0, 1), (0, 1), (0, 1),
                  (1, 2), (1, 3), (1, 9), (1, 6), (1, 18), (1, 10),
                  (1, 14), (1, 17), (1, 19), (1, 20), (1, 21), (1, 22),
                  (1, 23), (1, 24), (1, 8))
        if not 0 <= field < len(fields): return False, "配置字段编号无效"
        if field == 13:
            return False, "GR 是无效的旧配置；请在上位机修改 output_reduction"
        with self.lock:
            ranges = self.mit_ranges
            current_limit = self.mit_current_limit
            torque_constant = self.mit_torque_constant
        try:
            ranges, current_limit, torque_constant = updated_firmware_mit_protocol(
                ranges, current_limit, torque_constant, field, float(value))
        except ValueError as exc:
            return False, str(exc)
        import struct
        word = struct.unpack('<I', struct.pack('<f', float(value)))[0] if fields[field][0] else int(value) & 0xffffffff
        for offset in range(4):
            ok, msg = self.send_diag(0x07, 0x20 + field * 4 + offset, (word >> (8 * offset)) & 0xff)
            if not ok: return False, msg
            time.sleep(0.025)
        ok, msg = self.send_diag(0x07, 0xF8)
        if not ok: return False, msg
        if commit:
            time.sleep(0.025)
            ok, msg = self.send_diag(0x07, 0xF9)
            if not ok: return False, msg
            if field in (7, 12, 14, 15, 16, 17, 18, 19):
                try:
                    persist_mit_protocol(ranges, current_limit, torque_constant)
                except (OSError, ValueError, TypeError) as exc:
                    return False, f"固件已提交，但 WebUI MIT 范围未能持久化: {exc}"
                with self.lock:
                    self.mit_ranges = ranges
                    self.mit_current_limit = current_limit
                    self.mit_torque_constant = torque_constant
                return True, ("固件 MIT 协议清单已同步提交；"
                              f"p={ranges.position_min:g}..{ranges.position_max:g} rad, "
                              f"v={ranges.velocity_min:g}..{ranges.velocity_max:g} rad/s, "
                              f"Kp<= {ranges.kp_max:g}, Kd<= {ranges.kd_max:g}, "
                              f"t<= {ranges.torque_max:g} Nm")
            return True, "配置已提交；涉及 CAN/外设初始化的参数需重启生效"
        return True, "配置已暂存并请求校验；点击提交后才写入 Flash"

    def send_diag_action(self, name: str) -> tuple[bool, str]:
        if name == "ping":
            requests = [(0x00, 0, 0)]
        elif name == "snapshot":
            requests = [(0x02, page, 0) for page in
                        (0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                         82, 83, 84, 85, 86, 87, 90, 91, 92, 93, 94)]
            requests += [(0x03, page, 0) for page in (9, 10, 11, 12, 13)]
        elif name == "drv-status":
            requests = [(0x02, page, 0) for page in
                        (2, 3, 24, 25, 26, 27, 28, 29, 30, 56, 57, 58, 59, 60,
                         61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73,
                         74, 75, 76, 77, 78, 79, 80, 81)]
        elif name == "drv-fault-snapshot":
            requests = [(0x02, page, 0) for page in (2, 3, 123, 124, 125, 126, 127, 128, 129,
                                                       130, 131, 132, 133, 134, 135, 136)]
        elif name == "debug-on":
            requests = [(0x07, 7, 0)]
        elif name == "debug-off":
            requests = [(0x07, 8, 0)]
        elif name == "debug-status":
            requests = [(0x02, 99, 0)]
        else:
            return False, f"未知诊断动作: {name}"
        for opcode, page, argument in requests:
            ok, message = self.send_diag(opcode, page, argument)
            if not ok:
                return False, message
            # Firmware deliberately accepts at most one diagnostic request per
            # 20 ms.  A shorter interval deterministically drops alternate
            # pages, including the enable readback evidence required to decide
            # whether a motion failure is PWM, gate-driver or FOC related.
            time.sleep(0.025)
        return True, f"已通过 CAN0 桥接发送诊断动作: {name}（{len(requests)} 个请求）"

    def stop_mit(self) -> tuple[bool, str]:
        with self.lock:
            self.mit_stop_event.set()
            tty = self.bridge_tty
        if not tty:
            return False, "CAN0 trace 未运行，无法发送 MIT 停止帧"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(STOP_FRAME)
            self.log("MIT 停止帧已发送: ID=0x001 data=FF FF FF FF FF FF FF FD")
        except (OSError, TimeoutError) as exc:
            self.log(f"MIT 停止帧发送失败: {exc}")
            return False, f"MIT 停止帧发送失败: {exc}"
        return True, "已发送 MIT 停止帧"

    def start_mit_session(self, values: dict[str, Any]) -> tuple[bool, str]:
        with self.lock:
            if self.enable_active or self.mit_check_active:
                return False, "已有 CAN/MIT 动作运行中"
            self.enable_active = True
            self.mit_stop_event.clear()
            self.mit_session_frames = 0
            self.mit_thread = threading.Thread(
                target=self._mit_session_worker, args=(dict(values),), daemon=True)
            self.mit_thread.start()
        return True, "自定义 MIT 单次命令已提交"

    def _mit_session_worker(self, values: dict[str, Any]) -> None:
        try:
            values["_worker"] = True
            ok, message = self.mit_custom_once(values)
            self.log(message)
        finally:
            with self.lock:
                self.enable_active = False
                self.mit_thread = None

    def start_trajectory_session(self, values: dict[str, Any]) -> tuple[bool, str]:
        """Start the host-owned trajectory producer after a successful preflight."""
        with self.lock:
            ranges = self.mit_ranges
            reduction = self.output_reduction
        try:
            mode = str(values.get("mode", "position"))
            kp = float(values.get("kp"))
            kd = float(values.get("kd"))
            gravity_torque = float(values.get("gravity_torque", values.get("torque", 0.0)))
            friction_torque = float(values.get("friction_torque", 0.0))
            duration = float(values.get("duration"))
            hold = float(values.get("hold", 0.0))
        except (TypeError, ValueError):
            return False, "轨迹参数必须填写数字"
        if mode not in ("position", "velocity"):
            return False, "未知轨迹模式"
        for name, value, low, high in (
                ("电机侧 MIT Kp", kp, 0.0, ranges.kp_max),
                ("电机侧 MIT Kd", kd, 0.0, ranges.kd_max),
                ("电机侧重力补偿力矩", gravity_torque, -ranges.torque_max, ranges.torque_max),
                ("电机侧摩擦补偿幅值", friction_torque, 0.0, ranges.torque_max),
                ("执行时间", duration, 0.1, MIT_MAX_DURATION_S),
                ("保持时间", hold, 0.0, MIT_MAX_DURATION_S)):
            if not math.isfinite(value) or not low <= value <= high:
                return False, f"{name} 超出允许范围 {low:g}..{high:g}"
        if abs(gravity_torque) + friction_torque > ranges.torque_max:
            return False, "重力补偿与摩擦补偿叠加后可能超出固件力矩范围"
        try:
            if mode == "position":
                target = float(values.get("target"))
                speed_limit = float(values.get("speed_limit"))
                if not (math.isfinite(target) and
                        ranges.position_min <= target * reduction <= ranges.position_max):
                    return False, ("输出端目标位置超出可编码范围 "
                                   f"{ranges.position_min / reduction:g}..{ranges.position_max / reduction:g} rad")
                output_speed_max = min(TRAJECTORY_MAX_SPEED_RAD_S,
                                       ranges.velocity_max / reduction)
                if not 0.005 <= speed_limit <= output_speed_max or not math.isfinite(speed_limit):
                    return False, f"输出端速度上限必须在 0.005..{output_speed_max:g} rad/s"
            else:
                velocity = float(values.get("velocity"))
                output_speed_max = min(TRAJECTORY_MAX_SPEED_RAD_S,
                                       ranges.velocity_max / reduction)
                if not 0.005 <= abs(velocity) <= output_speed_max or not math.isfinite(velocity):
                    return False, f"输出端速度必须在 +/-0.005..{output_speed_max:g} rad/s"
                if kp != 0.0:
                    return False, "恒速模式固定使用 Kp=0；位置环不能参与连续旋转"
        except (TypeError, ValueError):
            return False, "轨迹目标或速度必须填写数字"
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            start = self.last_feedback_position_rad
            if self.enable_active or self.mit_check_active:
                return False, "已有 CAN/MIT 动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace"
            if start is None:
                return False, "未取得当前位置；请重新执行动作前置检查"
            start_output = start / reduction
            if mode == "position":
                minimum_duration = 1.5 * abs(target - start_output) / speed_limit
                if duration < minimum_duration:
                    return False, (f"执行时间过短：该 S 曲线至少需要 {minimum_duration:.3f} 秒，"
                                   f"才能不超过 {speed_limit:g} rad/s")
            else:
                # Velocity mode is continuous.  The encoded MIT position is
                # rebased around live feedback on every frame; logical multi-
                # turn position is tracked independently on the host.
                target = start_output
            self.enable_active = True
            self.mit_stop_event.clear()
            self.mit_session_frames = 0
            if mode == "velocity":
                # The MIT feedback position is finite, while the velocity
                # feedback remains meaningful after it reaches an endpoint.
                # Rebase the host-owned multi-turn coordinate at each session.
                self.logical_position_rad = start
                self._logical_feedback_position_rad = start
                self._logical_tracking_active = True
            request = {"mode": mode, "kp": kp, "kd": kd,
                       "gravity_torque": gravity_torque, "friction_torque": friction_torque,
                       "duration": duration, "hold": hold, "start": start_output,
                       "target": target, "ranges": ranges, "reduction": reduction}
            if mode == "position":
                request["speed_limit"] = speed_limit
            else:
                request["velocity"] = velocity
            self.mit_thread = threading.Thread(
                target=self._trajectory_session_worker, args=(request,), daemon=True)
            self.mit_thread.start()
        return True, "上位机轨迹会话已启动"

    def _trajectory_session_worker(self, request: dict[str, Any]) -> None:
        mode = str(request["mode"])
        start = float(request["start"])
        target = float(request["target"])
        duration = float(request["duration"])
        hold = float(request["hold"])
        kp, kd = (float(request[name]) for name in ("kp", "kd"))
        gravity_torque = float(request["gravity_torque"])
        friction_torque = float(request["friction_torque"])
        reduction = float(request["reduction"])
        with self.lock:
            tty = self.bridge_tty
        ranges = request["ranges"]
        if not isinstance(ranges, MitRanges):
            self.log("轨迹会话取消：MIT 范围无效")
            return
        try:
            if not tty:
                self.log("轨迹会话取消：CAN0 Trace 已停止")
                return
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self._begin_motion_evidence(
                    "trajectory-" + mode, reduction=reduction,
                    output_target=target if mode == "position" else None,
                    motor_target=target * reduction if mode == "position" else None,
                    kp=kp, kd=kd, torque=gravity_torque)
                self.log("S 曲线轨迹: " if mode == "position" else "恒速轨迹: ")
                self.log(f"输出端起点={start:.4f} rad, 终点={target:.4f} rad, 执行={duration:g} s, "
                         f"Kp={kp:g}, Kd={kd:g}, 重力补偿={gravity_torque:g}, 摩擦补偿幅值={friction_torque:g}; 减速比={reduction:g}:1")
                started = time.monotonic()
                next_send = started
                last_send = None
                max_gap = 0.0
                while not self.mit_stop_event.is_set():
                    elapsed = time.monotonic() - started
                    if elapsed >= duration:
                        break
                    if mode == "position":
                        u = max(0.0, min(1.0, elapsed / duration))
                        blend = u * u * (3.0 - 2.0 * u)
                        position = start + (target - start) * blend
                        velocity = (target - start) * 6.0 * u * (1.0 - u) / duration
                    else:
                        velocity = float(request["velocity"])
                        with self.lock:
                            feedback_position = self.last_feedback_position_rad
                        # Kp is deliberately zero in continuous velocity mode;
                        # p_des is a bounded transport placeholder only.
                        position = start if feedback_position is None else feedback_position / reduction
                    effective_torque = compose_feedforward_torque(
                        gravity_torque, friction_torque, velocity, target - position)
                    motor_position, motor_velocity, motor_kp, motor_kd, motor_torque = output_to_motor(
                        position, velocity, kp, kd, effective_torque, reduction)
                    motor_position = min(ranges.position_max, max(ranges.position_min, motor_position))
                    now = time.monotonic()
                    if now < next_send:
                        time.sleep(next_send - now)
                    now = time.monotonic()
                    payload = encode_command(
                        motor_position, motor_velocity, motor_kp, motor_kd, motor_torque, ranges)
                    serial_port.write(format_slcan(1, payload))
                    self._record_motion_frame(motor_position, motor_velocity, motor_kp,
                                              motor_kd, motor_torque, payload)
                    if last_send is not None:
                        max_gap = max(max_gap, now - last_send)
                    last_send = now
                    self.mit_session_frames += 1
                    next_send += MIT_KEEPALIVE_INTERVAL_S
                    if next_send < now:
                        next_send = now + MIT_KEEPALIVE_INTERVAL_S
                hold_deadline = time.monotonic() + hold
                next_send = time.monotonic()
                while time.monotonic() < hold_deadline and not self.mit_stop_event.is_set():
                    now = time.monotonic()
                    if now < next_send:
                        time.sleep(next_send - now)
                    now = time.monotonic()
                    hold_position = target
                    if mode == "velocity":
                        with self.lock:
                            hold_position = (target if self.last_feedback_position_rad is None
                                             else self.last_feedback_position_rad / reduction)
                    effective_torque = compose_feedforward_torque(
                        gravity_torque, friction_torque, 0.0, target - hold_position)
                    motor_position, _, motor_kp, motor_kd, motor_torque = output_to_motor(
                        hold_position, 0.0, kp, kd, effective_torque, reduction)
                    motor_position = min(ranges.position_max, max(ranges.position_min, motor_position))
                    payload = encode_command(motor_position, 0.0, motor_kp, motor_kd,
                                             motor_torque, ranges)
                    serial_port.write(format_slcan(1, payload))
                    self._record_motion_frame(motor_position, 0.0, motor_kp,
                                              motor_kd, motor_torque, payload)
                    self.mit_session_frames += 1
                    next_send += MIT_KEEPALIVE_INTERVAL_S
                    if next_send < now:
                        next_send = now + MIT_KEEPALIVE_INTERVAL_S
                serial_port.write(STOP_FRAME)
                self.log("轨迹会话已发送停止帧（0xFD）")
                self.log(f"轨迹发送统计：{self.mit_session_frames} 帧，最大帧间隔 {max_gap * 1000:.1f} ms")
                self._finish_motion_evidence(max_gap)
        except (OSError, TimeoutError, ValueError) as exc:
            self.log(f"轨迹会话写入失败: {exc}")
        finally:
            stopped = self.mit_stop_event.is_set()
            self.log("轨迹会话结束；" + ("用户已停止" if stopped else "达到设定执行/保持时间"))
            with self.lock:
                self.enable_active = False
                self.mit_thread = None
                self._logical_tracking_active = False

    def start_action(self, name: str, command: list[str]) -> tuple[bool, str]:
        with self.lock:
            if self.active is not None:
                return False, f"已有动作运行中: {self.active['name']}"
            if command and Path(command[0]).resolve() == DIAG.resolve() and \
                    self.bridge is not None and self.bridge.poll() is None:
                return False, "UC12 正被 CAN0 Trace 占用；请先停止桥接再执行诊断"
            self.active = {"name": name, "command": command, "started": time.time()}
            self.last_result = {"state": "running", "exit_code": None, "name": name}
        self.log("$ " + shlex.join(command))
        thread = threading.Thread(target=self._run_action, args=(name, command), daemon=True)
        thread.start()
        return True, f"已启动: {name}"

    def _run_action(self, name: str, command: list[str]) -> None:
        exit_code = 127
        try:
            process = subprocess.Popen(
                command, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
            )
            assert process.stdout is not None
            for line in process.stdout:
                self.log(line)
            exit_code = process.wait()
        except OSError as exc:
            self.log(f"无法启动: {exc}")
        finally:
            self.log(f"动作结束: {name}; exit={exit_code}")
            with self.lock:
                self.active = None
                self.last_result = {"state": "finished", "exit_code": exit_code, "name": name}

    def start_bridge(self) -> tuple[bool, str]:
        with self.lock:
            if self.bridge is not None and self.bridge.poll() is None:
                return False, "CAN0 trace 桥接已运行"
            if self.active is not None:
                return False, f"已有动作运行中: {self.active['name']}"
            if not BRIDGE.is_file():
                return False, f"桥接程序不存在: {BRIDGE}"
        owners = self._uc12_tool_owners()
        if owners:
            return False, "UC12 被以下本机进程占用，请先停止：" + "; ".join(owners)
        with self.lock:
            self.bridge_tty = ""
            self.bridge = subprocess.Popen(
                [str(BRIDGE), "--channel", "0", "--unsafe-tx", "--trace", "--quiet-tx"],
                cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
            )
            thread = threading.Thread(target=self._read_bridge, daemon=True)
            thread.start()
        # Runtime diagnostics share the UC12 CAN queue with MIT traffic. They
        # remain available through send_diag_action(), but must be requested
        # explicitly instead of continuously competing with motion control.
        self.log("$ " + shlex.join([str(BRIDGE), "--channel", "0", "--unsafe-tx", "--trace", "--quiet-tx"]))
        return True, "CAN0 trace 桥接已启动，等待伪串口路径"

    def _read_bridge(self) -> None:
        assert self.bridge is not None and self.bridge.stdout is not None
        process = self.bridge
        for line in process.stdout:
            clean = line.rstrip("\n")
            # During motion, retain only feedback position updates. Parsing
            # and formatting every trace line competes with the TX thread.
            if QUIET_TRACE and clean.startswith("TRACE ") and "TRACE CAN RX " not in clean:
                continue
            if not (QUIET_TRACE and clean.startswith("TRACE CAN RX ")):
                self.log("BRIDGE " + clean)
            with self.lock:
                ranges = self.mit_ranges
            semantic = _semantic_can_line(clean, ranges)
            if semantic and not QUIET_TRACE:
                self.log("BRIDGE " + semantic)
            match = FEEDBACK_RE.search(clean)
            if match:
                payload = bytes.fromhex(match.group(1))
                if len(payload) == 6:
                    decoded = decode_feedback(payload, ranges)
                    with self.lock:
                        self.last_feedback_position = payload[1:3].hex().upper()
                        self.last_feedback_position_rad = float(decoded["position"])
                        self.last_feedback_velocity_rad_s = float(decoded["velocity"])
                        self.last_feedback_torque_nm = float(decoded["torque"])
                        evidence = self.last_motion_evidence
                        if evidence is not None and not evidence["finished"]:
                            position = self.last_feedback_position_rad
                            evidence["feedback_samples"] += 1
                            minimum = evidence["feedback_motor_position_min_rad"]
                            maximum = evidence["feedback_motor_position_max_rad"]
                            evidence["feedback_motor_position_min_rad"] = (
                                position if minimum is None else min(minimum, position))
                            evidence["feedback_motor_position_max_rad"] = (
                                position if maximum is None else max(maximum, position))
                            evidence["feedback_torque_peak_nm"] = max(
                                float(evidence["feedback_torque_peak_nm"]),
                                abs(self.last_feedback_torque_nm))
                        if self.logical_position_rad is None:
                            self.logical_position_rad = self.last_feedback_position_rad
                        elif (self._logical_tracking_active and
                              self._logical_feedback_position_rad is not None):
                            self.logical_position_rad += feedback_position_delta(
                                self._logical_feedback_position_rad,
                                self.last_feedback_position_rad,
                                ranges,
                            )
                        self._logical_feedback_position_rad = self.last_feedback_position_rad
                        self.feedback_generation += 1
            if clean.startswith("Serial Port: "):
                self.bridge_tty = clean.removeprefix("Serial Port: ").strip()
        code = process.wait()
        self.log(f"CAN0 trace 桥接已退出; exit={code}")
        with self.lock:
            if self.bridge is process:
                self.bridge = None
                self.bridge_tty = ""

    def stop_bridge(self) -> tuple[bool, str]:
        with self.lock:
            if self.bridge is None or self.bridge.poll() is not None:
                return False, "CAN0 trace 桥接未运行"
            process = self.bridge
            process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.log("桥接未在 3 秒内退出，执行强制结束以释放 UC12")
            process.kill()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                return False, "桥接强制结束失败；请手动断开 USB-CAN/UC12"
            with self.lock:
                if self.bridge is process:
                    self.bridge = None
                    self.bridge_tty = ""
            return True, "桥接已强制停止并释放 UC12"
        return True, "CAN0 trace 桥接已停止并释放 UC12"

    def preflight_control(self) -> tuple[bool, str]:
        """Require a fresh neutral MIT feedback before any motion action."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            before = self.feedback_generation
            ranges = self.mit_ranges
        if not live or not tty:
            return False, "请先启动 CAN0 trace，并等待 Serial Port 路径出现"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(format_slcan(1, encode_command(0.0, 0.0, 0.0, 0.0, 0.0, ranges)))
            self.log("动作前置检查: 已发送中性 MIT 帧，等待新反馈")
        except (OSError, TimeoutError) as exc:
            return False, f"动作前置检查发送失败: {exc}"
        deadline = time.monotonic() + 0.25
        while time.monotonic() < deadline:
            with self.lock:
                if self.feedback_generation > before:
                    position = self.last_feedback_position
                    return True, f"动作前置检查通过；收到新反馈，当前位置编码 {position}"
            time.sleep(0.01)
        return False, "动作前置检查失败：未收到新的控制板反馈；未发送后续动作帧"

    @staticmethod
    def _uc12_tool_owners() -> list[str]:
        names = ("athena_diag_uc12", "uc12_slcan_bridge", "uc12_gvret_bridge",
                 "uc12_listen", "uc12_discover")
        try:
            # `pgrep -af` with an alternation can match its own shell command
            # and is prone to racing an orphaned bridge during WebUI restart.
            # Read the process table directly and match executable path tokens.
            result = subprocess.run(
                ["ps", "ax", "-o", "pid=,command="],
                text=True, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, check=False,
            )
        except OSError:
            return []
        own_pid = os.getpid()
        owners: list[str] = []
        for line in result.stdout.splitlines():
            fields = line.split(maxsplit=1)
            if not fields or not fields[0].isdigit() or int(fields[0]) == own_pid:
                continue
            command = fields[1] if len(fields) == 2 else ""
            executable = command.split(None, 1)[0] if command else ""
            base = os.path.basename(executable)
            if base in names:
                owners.append(line)
        return owners

    def mit_check_repeat(self) -> tuple[bool, str]:
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            ranges = self.mit_ranges
            if self.mit_check_active:
                return False, "MIT 三次验证已在运行"
            if self.enable_active:
                return False, "CAN 动作正在运行，请等待其完成"
            self.mit_check_active = True
        if not live or not tty:
            with self.lock:
                self.mit_check_active = False
            return False, "请先启动 CAN0 trace 桥接，并等待 Serial Port 路径出现"
        try:
            with self._open_serial(tty) as serial_port:
                for index in range(3):
                    serial_port.write(format_slcan(1, encode_command(0.0, 0.0, 0.0, 0.0, 0.0, ranges)))
                    self.log(f"MIT 非使能验证帧 {index + 1}/3 已发送: ID=0x001 DLC=8")
                    if index != 2:
                        time.sleep(0.2)
        except OSError as exc:
            with self.lock:
                self.mit_check_active = False
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.mit_check_active = False
        self.log("MIT 三次验证发送完成；请确认日志出现 3 条 TRACE CAN RX t000#... 且电机无动作")
        return True, "已发送 3 次固定 MIT 非使能验证帧（间隔 200 ms）"

    def enable_once(self) -> tuple[bool, str]:
        """Send exactly one 0xFC enable frame; never send a torque frame."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            if self.mit_check_active:
                return False, "MIT 三次验证正在运行，请等待其完成"
            if self.enable_active:
                return False, "单次受限使能已在发送"
            if not live or not tty:
                return False, "请先启动 CAN0 trace 桥接，并等待 Serial Port 路径出现"
            self.enable_active = True
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
            self.log("受限使能帧已发送一次: ID=0x001 DLC=8 data=FF FF FF FF FF FF FF FC")
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("使能请求已发送；请读取 DRV 状态/Snapshot，勿在确认前发送力矩帧")
        return True, "已发送一次受限使能帧（0xFC），未发送力矩命令"

    @staticmethod
    def _frame_from_feedback(position_hex: str, ranges: MitRanges, position_delta: float,
                             kp: float = 0.0, kd: float = 0.0,
                             torque: float = 0.0) -> str:
        """Build a command from firmware feedback with the same MIT contract."""
        current = ranges.position_min + (int(position_hex, 16) *
                                         (ranges.position_max - ranges.position_min) / 65535.0)
        target = min(ranges.position_max, max(ranges.position_min, current + position_delta))
        return format_slcan(1, encode_command(target, 0.0, kp, kd, torque, ranges))

    def hold_zero_once(self) -> tuple[bool, str]:
        """Enable, then send bounded zero-output MIT keepalives at last position."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证以获得位置反馈"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        zero_frame = self._frame_from_feedback(position, ranges, 0.0)
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"零输出保持: 已发送使能帧，复用当前位置编码 {position}")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(zero_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("零输出保持窗口结束；未发送非零 Kp/Kd/力矩")
        return True, "已完成 1 秒零输出保持验证，可读取 DRV/Snapshot"

    def hold_tiny_kp_once(self) -> tuple[bool, str]:
        """Hold the latest position with the smallest practical Kp for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        tiny_frame = self._frame_from_feedback(position, ranges, 0.0,
                                                kp=min(1.0, ranges.kp_max))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"极小闭环: 已发送使能帧，复用当前位置编码 {position}")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(tiny_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("极小闭环窗口结束；Kp 为最低测试档，Kd/前馈力矩均为零")
        return True, "已完成 1 秒极小 Kp 闭环验证"

    def feedforward_min_once(self) -> tuple[bool, str]:
        """Apply one quantization step of feed-forward torque for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        torque_frame = self._frame_from_feedback(
            position, ranges, 0.0, torque=(2.0 * ranges.torque_max / 4095.0))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"最小前馈力矩: 已发送使能帧，复用当前位置编码 {position}")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(torque_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("最小前馈力矩窗口结束；仅发送约 0.02 Nm 的一个量化步进")
        return True, "已完成 1 秒最小前馈力矩验证"

    def feedforward_low_once(self) -> tuple[bool, str]:
        """Apply a bounded approximately +0.2 Nm feed-forward torque for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        torque_frame = self._frame_from_feedback(position, ranges, 0.0,
                                                  torque=min(0.2, ranges.torque_max))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"低力矩: 已发送使能帧，复用当前位置编码 {position}，目标约 +0.2 Nm")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(torque_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("低力矩窗口结束；约 +0.2 Nm 测试已自动停止")
        return True, "已完成 1 秒约 +0.2 Nm 低力矩验证"

    def feedforward_medium_once(self) -> tuple[bool, str]:
        """Apply a bounded approximately +0.5 Nm feed-forward torque for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        torque_frame = self._frame_from_feedback(position, ranges, 0.0,
                                                  torque=min(0.5, ranges.torque_max))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"中低力矩: 已发送使能帧，复用当前位置编码 {position}，目标约 +0.5 Nm")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(torque_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("中低力矩窗口结束；约 +0.5 Nm 测试已自动停止")
        return True, "已完成 1 秒约 +0.5 Nm 低力矩验证"

    def feedforward_high_once(self) -> tuple[bool, str]:
        """Apply a bounded approximately +1.0 Nm feed-forward torque for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        torque_frame = self._frame_from_feedback(position, ranges, 0.0,
                                                  torque=min(1.0, ranges.torque_max))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"中等力矩: 已发送使能帧，复用当前位置编码 {position}，目标约 +1.0 Nm")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(torque_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("中等力矩窗口结束；约 +1.0 Nm 测试已自动停止")
        return True, "已完成 1 秒约 +1.0 Nm 力矩验证"

    def position_step_once(self) -> tuple[bool, str]:
        """Command a bounded +1 degree position step for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        position_frame = self._frame_from_feedback(position, ranges, math.pi / 180.0,
                                                    kp=min(5.0, ranges.kp_max))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"1°位置阶跃: {position} +1°，低 Kp，零前馈力矩")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(position_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("1°位置阶跃窗口结束；已自动停止")
        return True, "已完成 1 秒 1° 小角度位置阶跃验证"

    def position_step_stiffer_once(self) -> tuple[bool, str]:
        """Command the same +1 degree step with a modest Kp increase for 1 s."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        position_frame = self._frame_from_feedback(position, ranges, math.pi / 180.0,
                                                    kp=min(20.0, ranges.kp_max))
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"1°位置阶跃(Kp≈20): {position} +1°，零前馈力矩")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(position_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log("1°位置阶跃(Kp≈20)窗口结束；已自动停止")
        return True, "已完成 1 秒 Kp≈20 的 1°位置阶跃验证"

    def position_step_custom(self, kp_value: Any) -> tuple[bool, str]:
        try:
            kp = float(kp_value)
        except (TypeError, ValueError):
            return False, "Kp 必须是数字"
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            ranges = self.mit_ranges
            if not 0.0 <= kp <= ranges.kp_max:
                return False, f"Kp 必须在固件范围 0..{ranges.kp_max:g} 内"
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        position_frame = self._frame_from_feedback(position, ranges, math.pi / 180.0, kp=kp)
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"自定义 Kp 位置阶跃: Kp={kp:g}, {position} +1°，零前馈力矩")
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    serial_port.write(position_frame)
                    time.sleep(MIT_KEEPALIVE_INTERVAL_S)
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        self.log(f"自定义 Kp={kp:g} 位置阶跃窗口结束；已自动停止")
        return True, f"已完成 1 秒 1° 位置阶跃（Kp={kp:g}）"

    def mit_custom_once(self, values: dict[str, Any]) -> tuple[bool, str]:
        """Enable once and inject one complete raw MIT command.

        The command is deliberately not repeated or followed by 0xFD: this
        isolates the exact host frame. Firmware CAN_TIMEOUT remains responsible
        for expiry, while stop_mit() can explicitly withdraw it earlier.
        """
        names = ("position", "velocity", "kp", "kd")
        with self.lock:
            ranges = self.mit_ranges
        limits = ((ranges.position_min, ranges.position_max),
                  (ranges.velocity_min, ranges.velocity_max), (0.0, ranges.kp_max),
                  (0.0, ranges.kd_max))
        try:
            parsed = [float(values.get(name)) for name in names]
            gravity_torque = float(values.get("gravity_torque", values.get("torque", 0.0)))
            friction_torque = float(values.get("friction_torque", 0.0))
        except (TypeError, ValueError):
            return False, "MIT 五个参数都必须填写数字"
        for name, value, (low, high) in zip(names, parsed, limits):
            if not math.isfinite(value) or not low <= value <= high:
                return False, f"{name} 超出允许范围 {low}..{high}"
        for name, value, low, high in (("重力补偿力矩", gravity_torque, -ranges.torque_max, ranges.torque_max),
                                       ("摩擦补偿幅值", friction_torque, 0.0, ranges.torque_max)):
            if not math.isfinite(value) or not low <= value <= high:
                return False, f"{name} 超出允许范围 {low}..{high}"
        if abs(gravity_torque) + friction_torque > ranges.torque_max:
            return False, "重力补偿与摩擦补偿叠加后可能超出固件力矩范围"
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            if self.mit_check_active or (self.enable_active and not values.get("_worker")):
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace"
            # start_mit_session owns the session flag; this method is the worker.
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self._begin_motion_evidence(
                    "custom-mit-one-shot", reduction=1.0, output_target=None,
                    motor_target=parsed[0], kp=parsed[2], kd=parsed[3], torque=gravity_torque)
                with self.lock:
                    feedback_position = self.last_feedback_position_rad
                position_error = parsed[0] - (feedback_position if feedback_position is not None else parsed[0])
                effective_torque = compose_feedforward_torque(
                    gravity_torque, friction_torque, parsed[1], position_error)
                data = encode_command(parsed[0], parsed[1], parsed[2], parsed[3],
                                      effective_torque, ranges)
                serial_port.write(format_slcan(1, data))
                self._record_motion_frame(parsed[0], parsed[1], parsed[2], parsed[3],
                                          effective_torque, data)
                self.mit_session_frames = 1
                self._finish_motion_evidence(0.0)
                self.log("自定义 MIT 单次注入: " + _mit_command_details(data, ranges))
                self.log("未重复发送、未自动发送 0xFD；固件将在 CAN_TIMEOUT 后停机，可随时手动停止")
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        return True, "已发送 0xFC 使能帧和 1 帧自定义 MIT 命令"

    def status(self) -> dict[str, Any]:
        with self.lock:
            bridge_running = self.bridge is not None and self.bridge.poll() is None
            active = dict(self.active) if self.active else None
            mit_active = self.mit_check_active
            enable_active = self.enable_active
        return {
            "active": active,
            "last_result": self.last_result,
            "bridge_running": bridge_running,
            "bridge_tty": self.bridge_tty,
            "mit_active": mit_active,
            "enable_active": enable_active,
            "mit_session_frames": self.mit_session_frames,
            "last_feedback_motor_position_rad": self.last_feedback_position_rad,
            "last_feedback_motor_velocity_rad_s": self.last_feedback_velocity_rad_s,
            "last_feedback_motor_torque_nm": self.last_feedback_torque_nm,
            "last_feedback_output_position_rad": None if self.last_feedback_position_rad is None else self.last_feedback_position_rad / self.output_reduction,
            "last_feedback_output_velocity_rad_s": None if self.last_feedback_velocity_rad_s is None else self.last_feedback_velocity_rad_s / self.output_reduction,
            "logical_motor_position_rad": self.logical_position_rad,
            "logical_output_position_rad": None if self.logical_position_rad is None else self.logical_position_rad / self.output_reduction,
            "logical_output_turns": None if self.logical_position_rad is None else self.logical_position_rad / (self.output_reduction * 2.0 * math.pi),
            "output_reduction": self.output_reduction,
            "mit_position_min": self.mit_ranges.position_min,
            "mit_position_max": self.mit_ranges.position_max,
            "mit_velocity_min": self.mit_ranges.velocity_min,
            "mit_velocity_max": self.mit_ranges.velocity_max,
            "mit_kp_max": self.mit_ranges.kp_max,
            "mit_kd_max": self.mit_ranges.kd_max,
            "mit_torque_max": self.mit_ranges.torque_max,
            "normal_sha": normal_firmware_config()[0],
            "normal_image": str(normal_firmware_config()[1]),
            "last_motion_evidence": (dict(self.last_motion_evidence)
                                     if self.last_motion_evidence is not None else None),
            "logs": list(self.logs),
        }


RUNNER = Runner()
ACCESS_TOKEN = ""


def local_addresses() -> list[str]:
    found: set[str] = set()
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = item[4][0]
            if not address.startswith("127."):
                found.add(address)
    except socket.gaierror:
        pass
    return sorted(found)


def actions() -> dict[str, tuple[str, list[str], bool]]:
    build = [
        "make", "release-normal", f"GCC_PATH={TOOLCHAIN}", "-j4",
    ]
    normal_sha, _ = normal_firmware_config()
    return {
    "offline-tests": ("离线主机测试", ["make", "host-test", "host-app-test", "host-tools-test"], False),
    "build-normal": ("重新构建正常固件", build, False),
        "flash-normal": (
            "刷入正常固件",
            [str(FLASH), "flash-normal", "--confirm-normal-sha", normal_sha,
             "--i-understand-this-writes-main-flash"], True,
        ),
        "boot-normal": ("启动正常固件", [str(FLASH), "boot-normal"], True),
        "diag-ping": ("正常固件兼容 PING", ["__bridge_diag__", "ping"], False),
        "diag-snapshot": ("诊断 Snapshot", ["__bridge_diag__", "snapshot"], False),
        "diag-drv-status": ("正常固件 DRV 状态", ["__bridge_diag__", "drv-status"], False),
        "diag-drv-fault-snapshot": ("读取 DRV 故障瞬间快照", ["__bridge_diag__", "drv-fault-snapshot"], False),
        "diag-debug-on": ("开启 RAM 调试日志", ["__bridge_diag__", "debug-on"], False),
        "diag-debug-off": ("关闭 RAM 调试日志", ["__bridge_diag__", "debug-off"], False),
        "diag-debug-status": ("读取 RAM 调试日志状态", ["__bridge_diag__", "debug-status"], False),
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "AthenaBench/1"

    def log_message(self, fmt: str, *args: object) -> None:
        message = fmt % args
        # The browser polls status continuously. Keep those transport details
        # out of the operator log so CAN evidence remains visible.
        if '"GET /api/status ' in message or '"POST /api/logs/clear ' in message:
            return
        RUNNER.log("HTTP " + message)

    def _authorized(self) -> bool:
        return self.headers.get("X-Bench-Token", "") == ACCESS_TOKEN

    def _json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _body(self) -> dict[str, Any]:
        size = int(self.headers.get("Content-Length", "0"))
        if size > 4096:
            raise ValueError("request too large")
        return json.loads(self.rfile.read(size) or b"{}")

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/":
            page = PAGE.encode("utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
            return
        if parsed.path == "/api/status":
            if not self._authorized():
                self._json(HTTPStatus.UNAUTHORIZED, {"error": "invalid token"})
                return
            self._json(HTTPStatus.OK, RUNNER.status())
            return
        self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:
        if not self._authorized():
            self._json(HTTPStatus.UNAUTHORIZED, {"error": "invalid token"})
            return
        try:
            body = self._body()
        except (ValueError, json.JSONDecodeError) as exc:
            self._json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return
        parsed = urlparse(self.path)
        control_paths = {
            "/api/bridge/enable-once", "/api/bridge/hold-zero",
            "/api/bridge/hold-tiny-kp", "/api/bridge/feedforward-min",
            "/api/bridge/feedforward-low", "/api/bridge/feedforward-medium",
            "/api/bridge/feedforward-high", "/api/bridge/position-step",
            "/api/bridge/position-step-stiff", "/api/bridge/position-step-custom",
            "/api/bridge/mit-custom", "/api/bridge/trajectory",
        }
        if parsed.path in control_paths and body.get("physical_ready"):
            ready, check_message = RUNNER.preflight_control()
            if not ready:
                self._json(HTTPStatus.CONFLICT, {"ok": False, "error": check_message})
                return
            RUNNER.log(check_message)
        if parsed.path == "/api/action":
            action = str(body.get("action", ""))
            entry = actions().get(action)
            if entry is None:
                self._json(HTTPStatus.BAD_REQUEST, {"error": "unknown action"})
                return
            label, command, requires_confirmation = entry
            if command and command[0] == "__bridge_diag__":
                ok, message = RUNNER.send_diag_action(command[1])
                self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT,
                           {"ok": ok, "message": message})
                return
            if requires_confirmation:
                if not body.get("physical_ready"):
                    self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认台架、限流和断电路径已就绪"})
                    return
                if action == "flash-normal" and body.get("sha", "").lower() != normal_firmware_config()[0]:
                    self._json(HTTPStatus.BAD_REQUEST, {"error": "SHA-256 未匹配当前镜像"})
                    return
            ok, message = RUNNER.start_action(label, command)
            self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})
            return
        if parsed.path == "/api/debug/log":
            try:
                index = int(body.get("index", -1))
            except (TypeError, ValueError):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "日志索引必须是 0..31 的整数"})
                return
            if not 0 <= index <= 31:
                self._json(HTTPStatus.BAD_REQUEST, {"error": "日志索引必须是 0..31"})
                return
            ok, message = RUNNER.send_debug_log(index)
            self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})
            return
        if parsed.path == "/api/config/set":
            try:
                field = int(body.get("field")); value = float(body.get("value")); commit = bool(body.get("commit"))
            except (TypeError, ValueError):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "配置字段和值无效"})
                return
            ok, message = RUNNER.send_config_set(field, value, commit)
            self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})
            return
        if parsed.path == "/api/output-reduction":
            ok, message = RUNNER.set_output_reduction(body.get("reduction"))
            self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})
            return
        if parsed.path == "/api/bridge/start":
            ok, message = RUNNER.start_bridge()
        elif parsed.path == "/api/bridge/stop":
            ok, message = RUNNER.stop_bridge()
        elif parsed.path == "/api/bridge/mit-check":
            ok, message = RUNNER.mit_check_repeat()
        elif parsed.path == "/api/bridge/mit-stop":
            ok, message = RUNNER.stop_mit()
        elif parsed.path == "/api/bridge/enable-once":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.enable_once()
        elif parsed.path == "/api/bridge/hold-zero":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.hold_zero_once()
        elif parsed.path == "/api/bridge/hold-tiny-kp":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.hold_tiny_kp_once()
        elif parsed.path == "/api/bridge/feedforward-min":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.feedforward_min_once()
        elif parsed.path == "/api/bridge/feedforward-low":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.feedforward_low_once()
        elif parsed.path == "/api/bridge/feedforward-medium":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.feedforward_medium_once()
        elif parsed.path == "/api/bridge/feedforward-high":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认电机已固定、限流已设置且可立即断电"})
                return
            ok, message = RUNNER.feedforward_high_once()
        elif parsed.path == "/api/bridge/position-step":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认机械运动空间、限流和断电路径均已就绪"})
                return
            ok, message = RUNNER.position_step_once()
        elif parsed.path == "/api/bridge/position-step-stiff":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认机械运动空间、限流和断电路径均已就绪"})
                return
            ok, message = RUNNER.position_step_stiffer_once()
        elif parsed.path == "/api/bridge/position-step-custom":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认机械运动空间、限流和断电路径均已就绪"})
                return
            ok, message = RUNNER.position_step_custom(body.get("kp"))
        elif parsed.path == "/api/bridge/mit-custom":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认机械运动空间、限流和断电路径均已就绪"})
                return
            ok, message = RUNNER.start_mit_session(body)
        elif parsed.path == "/api/bridge/trajectory":
            if not body.get("physical_ready"):
                self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认机械运动空间、限流和断电路径均已就绪"})
                return
            ok, message = RUNNER.start_trajectory_session(body)
            if not ok:
                RUNNER.log(f"轨迹请求被拒绝: {message}")
        elif parsed.path == "/api/logs/clear":
            RUNNER.clear_logs()
            self._json(HTTPStatus.OK, {"ok": True, "message": "已清除当前实时日志"})
            return
        else:
            self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})


PAGE = r'''<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Athena Bench</title>
<style>
:root{color-scheme:dark;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#111827;color:#e5e7eb}body{margin:0}.wrap{max-width:1180px;margin:auto;padding:14px 18px}header{display:flex;gap:20px;justify-content:space-between;align-items:end;border-bottom:1px solid #374151;padding-bottom:12px}h1{font-size:22px;margin:0}h2{font-size:15px;margin:0 0 8px}.muted{color:#9ca3af;font-size:12px}.summary{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin:10px 0;color:#cbd5e1;font-size:12px}.summary code{background:#0b1220;border:1px solid #374151;border-radius:4px;padding:4px 6px;color:#93c5fd}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:10px;margin:10px 0}.panel{border:1px solid #374151;border-radius:6px;padding:11px;background:#172033}.panel p{font-size:12px;line-height:1.35;color:#cbd5e1;margin:6px 0}.command{background:#0b1220;padding:7px;border-radius:4px;font:11px ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre-wrap;overflow-wrap:anywhere}button{border:1px solid #64748b;border-radius:4px;background:#1e3a5f;color:white;padding:7px 9px;font-size:13px;cursor:pointer}button.danger{background:#7f1d1d;border-color:#ef4444}button.secondary{background:#263449}button:disabled{opacity:.5;cursor:not-allowed}input{width:100%;box-sizing:border-box;background:#0b1220;border:1px solid #475569;color:#e5e7eb;border-radius:4px;padding:7px;margin:5px 0}.check{display:flex;gap:8px;align-items:start;font-size:12px;margin:7px 0}.check input{width:auto;margin:2px 0}#notice{min-height:18px;color:#fcd34d;font-size:13px}.log-head{display:flex;justify-content:space-between;align-items:start;gap:12px}.log-head h2{margin-top:8px}pre{height:330px;overflow:auto;margin:0;background:#050a14;border:1px solid #374151;border-radius:6px;padding:10px;white-space:pre-wrap;word-break:break-word;font:11px ui-monospace,SFMono-Regular,Menlo,monospace}.state{color:#93c5fd;font-size:12px}@media(max-width:550px){header{display:block}.wrap{padding:12px}pre{height:280px}}
</style><body><main class="wrap"><header><div><h1>Athena 电机控制台架</h1><div class="muted">固定动作面板。无任意 Shell/CAN 命令入口。</div></div><div id="state" class="state">等待连接</div></header>
<p id="notice"></p><div class="summary"><span>当前正常固件 SHA-256</span><code id="shaTop">加载中…</code><span>最近反馈位置</span><code id="positionRad">暂无</code><span>逻辑多圈位置</span><code id="logicalPosition">暂无</code><span>圈数</span><code id="logicalTurns">暂无</code><span>推荐顺序：刷写 → 启动 → PING → Snapshot → DRV 状态 → CAN Trace</span></div><section class="grid">
<article class="panel"><h2>离线验证</h2><p>构建前或代码修改后执行。不会访问控制板。</p><div class="command">make host-test host-app-test host-tools-test</div><p><button data-action="offline-tests">执行离线主机测试</button></p><div class="command">make release-normal GCC_PATH=&lt;固定 Arm GNU Toolchain&gt;</div><p><button data-action="build-normal">重新构建正常固件</button></p></article>
<article class="panel"><h2>刷写与启动</h2><p>刷写使用逐页擦写、写入、读回校验，并保留 CPU halted。启动前不发送任何运动命令。</p><div class="command">tools/athena_safe_flash.sh flash-normal --confirm-normal-sha <span id="sha"></span> --i-understand-this-writes-main-flash</div><input id="shaInput" aria-label="SHA-256" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="可手动粘贴完整 SHA-256"><p><button class="secondary" id="fillSha" type="button">填入当前 SHA</button></p><label class="check"><input id="physical" type="checkbox">我已确认控制板、ST-LINK、限流电源、机械固定和可断电路径均已就绪。</label><button class="danger" id="flash">刷入正常固件</button><hr><div class="command">tools/athena_safe_flash.sh boot-normal</div><label class="check"><input id="bootReady" type="checkbox">我已确认物理台架可安全启动。</label><button id="boot">启动正常固件</button></article>
<article class="panel"><h2>正常固件只读验证</h2><p>PING、Snapshot 和 DRV 状态均为只读，不会启用电机。</p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 ping</div><p><button data-action="diag-ping">执行 PING</button></p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 snapshot</div><p><button data-action="diag-snapshot">执行 Snapshot</button></p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 drv-status</div><p><button data-action="diag-drv-status">读取 DRV 状态</button> <button class="secondary" data-action="diag-drv-fault-snapshot">读取故障瞬间快照</button></p><hr><h2>RAM 调试日志</h2><p>默认关闭；开启后只记录运行期关键事件，重启会清空。读取前请停止 CAN0 Trace。</p><p><button data-action="diag-debug-on">开启记录</button> <button class="secondary" data-action="diag-debug-status">读取状态</button> <button class="secondary" data-action="diag-debug-off">关闭记录</button></p><div style="display:flex;gap:6px;align-items:center"><input id="debugLogIndex" type="number" min="0" max="31" step="1" value="0" aria-label="RAM 调试日志索引"><button id="debugLogRead" class="secondary">读取该条日志</button></div></article>
<article class="panel"><h2>CAN0 收发与受限使能</h2><p>桥接独占 UC12。非使能验证帧由当前固件 MIT 协议清单编码；确认收到控制板回复后，才可在电机固定、限流和断电路径就绪时发送一次 0xFC。</p><div class="command">./uc12_slcan_bridge --channel 0 --unsafe-tx --trace</div><p><button id="bridgeStart">启动 CAN0 Trace</button> <button class="secondary" id="bridgeStop">停止</button></p><p><button id="mitCheck">发送三次 MIT 非使能验证</button></p><div class="command">0xFC 使能帧仅发送一次</div><label class="check"><input id="enableReady" type="checkbox">我已确认电机已固定、限流已设置，并能立即断电。</label><p><button class="danger" id="enableOnce">单次受限使能</button> <button id="holdZero">1 秒零输出保持</button> <button id="holdTinyKp">1 秒极小 Kp 闭环</button> <button id="feedforwardMin">1 秒最小力矩量化步进</button> <button id="feedforwardLow">1 秒约 0.2 Nm</button> <button id="feedforwardMedium">1 秒约 0.5 Nm</button> <button id="feedforwardHigh">1 秒约 1.0 Nm</button> <button id="positionStep">1 秒 1° 位置阶跃</button> <button id="positionStepStiff">1 秒 1° 阶跃(Kp≈20)</button></p><p class="muted">力矩测试持续 1 秒自动停止。位置阶跃会尝试小幅运动，必须确认运动空间已释放、限流 0.2 A 且可立即断电。</p></article>
</section><p class="panel"><label>Kp（位置阶跃，0-100）：<input id="customKp" type="number" min="0" max="100" step="1" value="20"></label> <button id="positionStepCustom">执行输入 Kp 的 1° 阶跃</button></p><div class="log-head"><h2>实时日志</h2><button class="secondary" id="clearLogs">清除当前内容</button></div><pre id="log">等待认证…</pre></main><script>
const params=new URLSearchParams(location.search), fromUrl=params.get('token'); let token=fromUrl||localStorage.getItem('athenaBenchToken')||'';if(fromUrl)localStorage.setItem('athenaBenchToken',fromUrl);if(!token){token=prompt('输入服务启动时显示的访问令牌：')||'';localStorage.setItem('athenaBenchToken',token)}
const note=t=>document.querySelector('#notice').textContent=t;const api=async(path,body)=>{let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Bench-Token':token},body:JSON.stringify(body||{})});let j=await r.json();if(!r.ok)throw Error(j.error||j.message||r.status);return j};
async function action(name,extra={}){try{let j=await api('/api/action',{action:name,...extra});note(j.message)}catch(e){note('失败: '+e.message)}}
document.querySelector('#positionStepCustom').onclick=()=>api('/api/bridge/position-step-custom',{physical_ready:document.querySelector('#enableReady').checked,kp:document.querySelector('#customKp').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#debugLogRead').onclick=()=>api('/api/debug/log',{index:document.querySelector('#debugLogIndex').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));
document.querySelectorAll('[data-action]').forEach(b=>b.onclick=()=>action(b.dataset.action));document.querySelector('#fillSha').onclick=()=>{document.querySelector('#shaInput').value=document.querySelector('#sha').textContent;note('已填入当前镜像 SHA-256')};document.querySelector('#flash').onclick=()=>action('flash-normal',{physical_ready:document.querySelector('#physical').checked,sha:document.querySelector('#shaInput').value.trim()});document.querySelector('#boot').onclick=()=>action('boot-normal',{physical_ready:document.querySelector('#bootReady').checked});document.querySelector('#bridgeStart').onclick=()=>api('/api/bridge/start').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#bridgeStop').onclick=()=>api('/api/bridge/stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitCheck').onclick=()=>api('/api/bridge/mit-check').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#enableOnce').onclick=()=>api('/api/bridge/enable-once',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#holdZero').onclick=()=>api('/api/bridge/hold-zero',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#holdTinyKp').onclick=()=>api('/api/bridge/hold-tiny-kp',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardMin').onclick=()=>api('/api/bridge/feedforward-min',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardLow').onclick=()=>api('/api/bridge/feedforward-low',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardMedium').onclick=()=>api('/api/bridge/feedforward-medium',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardHigh').onclick=()=>api('/api/bridge/feedforward-high',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#positionStep').onclick=()=>api('/api/bridge/position-step',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#positionStepStiff').onclick=()=>api('/api/bridge/position-step-stiff',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#clearLogs').onclick=()=>api('/api/logs/clear').then(x=>{document.querySelector('#log').textContent='';note(x.message)}).catch(e=>note('失败: '+e.message));
async function refresh(){try{let r=await fetch('/api/status',{headers:{'X-Bench-Token':token}});if(!r.ok)throw Error('令牌无效');let s=await r.json();document.querySelector('#sha').textContent=s.normal_sha;document.querySelector('#shaTop').textContent=s.normal_sha;document.querySelector('#positionRad').textContent=s.last_feedback_position_rad===null?'暂无':Number(s.last_feedback_position_rad).toFixed(5)+' rad';let lp=document.querySelector('#logicalPosition'),lt=document.querySelector('#logicalTurns');if(lp)lp.textContent=s.logical_position_rad===null?'暂无':Number(s.logical_position_rad).toFixed(5)+' rad';if(lt)lt.textContent=s.logical_turns===null?'暂无':Number(s.logical_turns).toFixed(4);let busy=s.mit_active||s.enable_active;document.querySelector('#state').textContent=s.active?'正在执行: '+s.active.name:(busy?'CAN 动作运行中':(s.bridge_running?'CAN0 Trace 运行中 '+s.bridge_tty:'空闲'));['mitCheck','enableOnce','holdZero','holdTinyKp','feedforwardMin','feedforwardLow','feedforwardMedium','feedforwardHigh','positionStep','positionStepStiff','positionStepCustom','mitCustom'].forEach(id=>{let e=document.querySelector('#'+id);if(e)e.disabled=busy});let log=document.querySelector('#log'),nearEnd=log.scrollHeight-log.scrollTop-log.clientHeight<40;log.textContent=s.logs.join('\n');if(nearEnd)log.scrollTop=log.scrollHeight}catch(e){note('无法读取状态: '+e.message)}}refresh();setInterval(refresh,1200);
</script></body></html>'''


PAGE = PAGE.replace(
    '<span>最近反馈位置</span><code id="positionRad">暂无</code><span>逻辑多圈位置</span>',
    '<span>电机侧反馈</span><code id="motorPositionRad">暂无</code><span>输出端反馈</span><code id="outputPositionRad">暂无</code><span>输出端逻辑多圈</span>',
    1,
).replace(
    "document.querySelector('#positionRad').textContent=s.last_feedback_position_rad===null?'暂无':Number(s.last_feedback_position_rad).toFixed(5)+' rad';",
    "document.querySelector('#motorPositionRad').textContent=s.last_feedback_motor_position_rad===null?'暂无':Number(s.last_feedback_motor_position_rad).toFixed(5)+' rad';document.querySelector('#outputPositionRad').textContent=s.last_feedback_output_position_rad===null?'暂无':Number(s.last_feedback_output_position_rad).toFixed(5)+' rad';",
    1,
).replace(
    "s.logical_position_rad===null?'暂无':Number(s.logical_position_rad).toFixed(5)+' rad'",
    "s.logical_output_position_rad===null?'暂无':Number(s.logical_output_position_rad).toFixed(5)+' rad'",
    1,
).replace(
    "s.logical_turns===null?'暂无':Number(s.logical_turns).toFixed(4)",
    "s.logical_output_turns===null?'暂无':Number(s.logical_output_turns).toFixed(4)",
    1,
).replace(
    '<span>圈数</span><code id="logicalTurns">暂无</code>',
    '<span>输出端圈数</span><code id="logicalTurns">暂无</code>',
    1,
).replace(
    '<option value="13">GR</option>',
    '',
    1,
).replace(
    '<pre id="log">等待认证…</pre>', '<textarea id="log" readonly spellcheck="false">等待认证…</textarea>',
).replace(
    'pre{height:330px;overflow:auto;', 'textarea#log{height:330px;width:100%;box-sizing:border-box;resize:vertical;overflow:auto;',
).replace(
    'pre{height:280px}', 'textarea#log{height:280px}',
).replace(
    '<section class="grid">',
    '<nav class="tabs" role="tablist"><button class="tab active" data-tab-select="offline">离线验证</button><button class="tab" data-tab-select="flash">刷写与启动</button><button class="tab" data-tab-select="readonly">只读验证</button><button class="tab" data-tab-select="mit">自定义 MIT</button></nav><section class="grid">',
    1,
).replace(
    '<article class="panel"><h2>离线验证', '<article class="panel tab-panel" data-tab="offline"><h2>离线验证',
).replace(
    '<article class="panel"><h2>刷写与启动', '<article class="panel tab-panel" data-tab="flash"><h2>刷写与启动',
).replace(
    '<article class="panel"><h2>正常固件只读验证', '<article class="panel tab-panel" data-tab="readonly"><h2>正常固件只读验证',
).replace(
    '<article class="panel"><h2>自定义 MIT 持续会话', '<article class="panel tab-panel" data-tab="mit"><h2>自定义 MIT 持续会话',
).replace(
    '<article class="panel"><h2>CAN0 收发与受限使能', '<article class="panel can-strip"><h2>CAN0 收发与受限使能',
).replace(
    '<button id="holdZero">1 秒零输出保持</button>', '<select id="canActionSelect" aria-label="选择 CAN 测试动作"><option value="holdZero">1 秒零输出保持</option><option value="holdTinyKp">1 秒极小 Kp 闭环</option><option value="feedforwardMin">1 秒约 0.02 Nm</option><option value="feedforwardLow">1 秒约 0.2 Nm</option><option value="feedforwardMedium">1 秒约 0.5 Nm</option><option value="feedforwardHigh">1 秒约 1.0 Nm</option><option value="positionStep">1 秒 1° 位置阶跃</option><option value="positionStepStiff">1 秒 1° 阶跃(Kp≈20)</option></select><button id="canActionRun">执行所选 CAN 测试</button><button class="legacy-control" id="holdZero">1 秒零输出保持</button>',
).replace(
    '<button id="holdTinyKp">1 秒极小 Kp 闭环</button>', '<button class="legacy-control" id="holdTinyKp">1 秒极小 Kp 闭环</button>',
).replace(
    '<button id="feedforwardMin">1 秒约 0.02 Nm</button>', '<button class="legacy-control" id="feedforwardMin">1 秒约 0.02 Nm</button>',
).replace(
    '<button id="feedforwardLow">1 秒约 0.2 Nm</button>', '<button class="legacy-control" id="feedforwardLow">1 秒约 0.2 Nm</button>',
).replace(
    '<button id="feedforwardMedium">1 秒约 0.5 Nm</button>', '<button class="legacy-control" id="feedforwardMedium">1 秒约 0.5 Nm</button>',
).replace(
    '<button id="feedforwardHigh">1 秒约 1.0 Nm</button>', '<button class="legacy-control" id="feedforwardHigh">1 秒约 1.0 Nm</button>',
).replace(
    '<button id="positionStep">1 秒 1° 位置阶跃</button>', '<button class="legacy-control" id="positionStep">1 秒 1° 位置阶跃</button>',
).replace(
    '<button id="positionStepStiff">1 秒 1° 阶跃(Kp≈20)</button>', '<button class="legacy-control" id="positionStepStiff">1 秒 1° 阶跃(Kp≈20)</button>',
).replace(
    '</script></body></html>', "document.querySelectorAll('[data-tab-select]').forEach(b=>b.onclick=()=>{document.querySelectorAll('[data-tab-select]').forEach(x=>x.classList.toggle('active',x===b));document.querySelectorAll('[data-tab]').forEach(p=>p.hidden=p.dataset.tab!==b.dataset.tabSelect)});document.querySelectorAll('[data-tab]').forEach(p=>p.hidden=p.dataset.tab!=='offline');document.querySelector('#canActionRun').onclick=()=>document.querySelector('#'+document.querySelector('#canActionSelect').value).click();</script></body></html>",
).replace(
    '<style>', '<style>.tabs{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0}.tabs .tab{background:#172033;color:#cbd5e1}.tabs .tab.active{background:#2563eb;color:#fff}.tab-panel[hidden]{display:none}.panel label:not(.check){display:inline-flex;flex-direction:column;gap:3px;vertical-align:top;margin:4px 8px 4px 0;color:#cbd5e1;font-size:12px}.panel label:not(.check) input,.panel label:not(.check) select{min-width:130px}.can-strip{grid-column:1/-1}.can-strip p{display:flex;gap:6px;flex-wrap:wrap;align-items:center}.can-strip select{max-width:240px;width:auto;background:#0b1220;border:1px solid #475569;color:#e5e7eb;border-radius:4px;padding:7px}.legacy-control{display:none!important}',
).replace(
    "document.querySelector('#log').textContent='';", "document.querySelector('#log').value='';",
).replace(
    "log.textContent=s.logs.join('\\n');", "if(document.activeElement!==log||log.selectionStart===log.selectionEnd)log.value=s.logs.join('\\n');",
).replace(
    '1 秒零输出保持', 'MIT 保持：1 秒零输出（10 ms 心跳）',
).replace(
    '1 秒极小 Kp 闭环', 'MIT 保持：1 秒极小 Kp 闭环',
).replace(
    '再在电机固定、限流和断电路径确认后发送一次 0xFC。',
    '再在电机固定、限流和断电路径确认后发送一次 0xFC。单次使能不会持续刷新 CAN watchdog。',
).replace(
    '力矩测试持续 1 秒自动停止。',
    '除单次受限使能外，MIT 测试会以 10 ms 周期持续发送控制帧并保持反馈，1 秒后自动停止。',
).replace(
    "document.querySelectorAll('[data-action]').forEach",
    "document.querySelector('#mitCustom').onclick=()=>api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:document.querySelector('#mitP').value,velocity:document.querySelector('#mitV').value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,gravity_torque:document.querySelector('#mitGravity').value,friction_torque:document.querySelector('#mitFriction').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitStop').onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelectorAll('[data-action]').forEach",
).replace(
    '<span>当前正常固件 SHA-256</span>',
    '<span>当前正常固件 SHA-256</span>',
).replace(
    "document.querySelector('#shaTop').textContent=s.normal_sha;",
    "document.querySelector('#shaTop').textContent=s.normal_sha;",
)
PAGE = PAGE.replace(
    "document.querySelector('#mitCustom').onclick=()=>api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:document.querySelector('#mitP').value,velocity:document.querySelector('#mitV').value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,gravity_torque:document.querySelector('#mitGravity').value,friction_torque:document.querySelector('#mitFriction').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitStop').onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));",
    "const mitCustomButton=document.querySelector('#mitCustom'),mitStopButton=document.querySelector('#mitStop');if(mitCustomButton)mitCustomButton.onclick=()=>api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:document.querySelector('#mitP').value,velocity:document.querySelector('#mitV').value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,gravity_torque:document.querySelector('#mitGravity').value,friction_torque:document.querySelector('#mitFriction').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));if(mitStopButton)mitStopButton.onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));",
    1,
)
if 'id="mitP"' not in PAGE:
    _mit_panel = r'''<article class="panel tab-panel" data-tab="mit"><h2>自定义 MIT 单次命令（电机侧）</h2><p>仅发送一次使能和一次 MIT 命令；固件 CAN_TIMEOUT 后自动失效。</p><p><label>位置 p (rad)<input id="mitP" type="number" step="0.001" value="0"></label><label>速度 v (rad/s)<input id="mitV" type="number" step="0.001" value="0"></label><label>位置增益 Kp<input id="mitKp" type="number" step="0.001" value="0"></label><label>速度增益 Kd<input id="mitKd" type="number" step="0.001" value="0"></label><label>重力补偿 (Nm)<input id="mitGravity" type="number" step="0.001" value="0"></label><label>摩擦补偿幅值 (Nm)<input id="mitFriction" type="number" min="0" step="0.001" value="0"></label></p><button id="mitCustom">发送单次 MIT</button> <button class="danger" id="mitStop">发送停止帧</button></article>'''
    PAGE = PAGE.replace('<div class="log-head">', _mit_panel + '<div class="log-head">', 1)
PAGE = PAGE.replace(
    '<p>以下五个参数严格使用固件 MIT API 的电机侧单位，不包含减速器换算。</p>',
    '<p>增益和前馈力矩始终使用固件 MIT API 的电机侧单位。位置和速度可在输出端输入，由上位机换算后发送。</p><p class="control-row"><label>目标坐标<select id="mitCoordinate"><option value="output">输出端目标（自动换算）</option><option value="motor">电机侧原始 MIT</option></select></label><span id="mitReductionNote" class="muted"></span></p><p class="control-row" id="mitOutputInputs"><label>输出端目标位置 (rad)<input id="mitOutputP" type="number" step="0.001" value="0"></label><label>输出端目标速度 (rad/s)<input id="mitOutputV" type="number" step="0.001" value="0"></label></p>',
    1,
)
PAGE = PAGE.replace('<label>位置 p (rad)<input id="mitP"', '<label>电机侧位置 p (rad)<input id="mitP"', 1)
PAGE = PAGE.replace('<label>速度 v (rad/s)<input id="mitV"', '<label>电机侧速度 v (rad/s)<input id="mitV"', 1)
PAGE = PAGE.replace('<label>位置增益 Kp<input id="mitKp"', '<label>电机侧 MIT Kp<input id="mitKp"', 1)
PAGE = PAGE.replace('<label>速度增益 Kd<input id="mitKd"', '<label>电机侧 MIT Kd<input id="mitKd"', 1)
PAGE = PAGE.replace('<label>前馈力矩 (Nm)<input id="mitT"', '<label>电机侧重力补偿 (Nm)<input id="mitGravity" type="number" step="0.001" value="0"><label>电机侧摩擦补偿幅值 (Nm)<input id="mitFriction" type="number" min="0" step="0.001" value="0">', 1)
PAGE = PAGE.replace('<label>电机侧 MIT 前馈力矩 (Nm)<input id="mitT"', '<label>电机侧重力补偿 (Nm)<input id="mitGravity" type="number" step="0.001" value="0"><label>电机侧摩擦补偿幅值 (Nm)<input id="mitFriction" type="number" min="0" step="0.001" value="0">', 1)

# The fixed Kp position-step control is intentionally no longer exposed in the UI.
# Keep its backend endpoint for compatibility with older scripts, but remove its
# panel and click binding from the rendered page.
PAGE = re.sub(r'<p class="panel"><label>Kp（位置阶跃，0-100）：.*?</p>', '', PAGE, flags=re.S)
PAGE = re.sub(r"document\.querySelector\('#positionStepCustom'\)\.onclick=.*?;\n", '', PAGE)

# Upper-layer motion planning is deliberately separate from the raw five
# parameter MIT panel.  The raw panel remains useful for protocol/FOC checks;
# this panel owns the time-varying position and velocity references needed for
# repeatable motion tests.
_trajectory_panel = r'''<article class="panel tab-panel" data-tab="trajectory"><h2>上位机轨迹控制</h2><p>目标位置和速度以减速器输出端填写；页面按当前减速比换算为电机侧目标。Kp、Kd 和补偿力矩始终是固件 MIT API 的电机侧参数。</p><p><label>模式<select id="trajectoryMode" aria-label="轨迹模式"><option value="position">S 曲线到目标位置</option><option value="velocity">恒速前进</option></select></label></p><p id="trajectoryPositionInputs"><label>输出端目标位置 (rad)<input id="trajectoryTarget" type="number" step="0.001" placeholder="输出端目标位置"></label><output id="trajectoryTargetMotorPreview" class="muted">电机侧目标位置: 等待输入</output><label>输出端速度上限 (rad/s)<input id="trajectorySpeedLimit" type="number" min="0.005" max="7.2" step="0.005" value="0.02"></label><output id="trajectorySpeedLimitMotorPreview" class="muted">电机侧速度上限: 等待输入</output></p><p id="trajectoryVelocityInputs" hidden><label>输出端恒速 (rad/s)<input id="trajectoryVelocity" type="number" min="-7.2" max="7.2" step="0.005" value="0.02"></label><output id="trajectoryVelocityMotorPreview" class="muted">电机侧恒速: 等待输入</output></p><p><label>执行时间 (s)<input id="trajectoryDuration" type="number" min="0.1" max="300" step="0.1" value="2"></label><label>保持时间 (s)<input id="trajectoryHold" type="number" min="0" max="300" step="0.1" value="2"></label><label>电机侧 MIT Kp<input id="trajectoryKp" type="number" min="0" max="500" step="0.1" value="20"></label><label>电机侧 MIT Kd<input id="trajectoryKd" type="number" min="0" max="5" step="0.01" value="1"></label><label>电机侧重力补偿 (Nm)<input id="trajectoryGravity" type="number" step="0.01" value="0"></label><label>电机侧摩擦补偿幅值 (Nm)<input id="trajectoryFriction" type="number" min="0" step="0.01" value="0"></label></p><p class="muted">摩擦补偿指令与期望运动同向；零速保持时根据位置误差方向取同向符号。</p><button id="trajectoryStart">开始轨迹</button> <button class="danger" id="trajectoryStop">停止轨迹</button></article>'''
if 'id="trajectoryTarget"' not in PAGE:
    PAGE = PAGE.replace('<div class="log-head">', _trajectory_panel + '<div class="log-head">', 1)
PAGE = PAGE.replace(
    '<p><label>模式<select id="trajectoryMode" aria-label="轨迹模式">',
    '<p><label>减速比（电机转数 / 输出转数）<input id="outputReduction" type="number" min="1" max="1000" step="0.001" value="9"></label> <button class="secondary" id="outputReductionSave">保存减速比</button> <span class="muted">仅影响上位机 P/V 映射，不写入电机固件。</span></p><p><label>模式<select id="trajectoryMode" aria-label="轨迹模式">',
    1,
)
PAGE = PAGE.replace(
    '<span>最近反馈位置</span><code id="positionRad">暂无</code><span>逻辑多圈位置</span><code id="logicalPosition">暂无</code><span>圈数</span><code id="logicalTurns">暂无</code><span>推荐顺序：刷写 → 启动 → PING → Snapshot → DRV 状态 → CAN Trace</span>',
    '<span>电机侧反馈</span><code id="motorFeedback">暂无</code><span>输出端反馈</span><code id="outputFeedback">暂无</code><span>输出端逻辑多圈</span><code id="logicalOutputPosition">暂无</code><span>输出端圈数</span><code id="outputTurns">暂无</code>',
    1,
)
PAGE = re.sub(r'(<p id="notice"></p>)(<div class="summary">.*?</div>)(<nav class="tabs"[^>]*>.*?</nav>)',
              r'\1\3\2', PAGE, count=1, flags=re.S)
PAGE = PAGE.replace(
    'id="trajectoryVelocity" type="number" min="-20" max="20" step="0.05" value="1"',
    'id="trajectoryVelocity" type="number" min="-20" max="20" step="0.05" value="0.2"',
)
PAGE = PAGE.replace(
    '<button class="tab" data-tab-select="mit">自定义 MIT</button>',
    '<button class="tab" data-tab-select="mit">自定义 MIT</button><button class="tab" data-tab-select="trajectory">轨迹控制</button>',
    1,
)
PAGE = PAGE.replace(
    '</article></section><p class="panel">',
    '</article>' + _trajectory_panel + '<article class="panel tab-panel" data-tab="readonly"><h2>CAN 配置（兼容 UART Setup）</h2><p class="muted">配置通过 CAN 暂存；勾选提交后写入 Flash。UART Setup 仍可用，两者共用同一校验和事务保存。</p><p><select id="configField"><option value="6">I_BW</option><option value="7">I_MAX</option><option value="8">I_MAX_CONT</option><option value="9">I_FW_MAX</option><option value="10">I_CAL</option><option value="11">PPAIRS</option><option value="12">KT</option><option value="13">GR</option><option value="14">P_MIN</option><option value="15">P_MAX</option><option value="16">V_MIN</option><option value="17">V_MAX</option><option value="18">KP_MAX</option><option value="19">KD_MAX</option><option value="20">TEMP_MAX</option><option value="0">PHASE_ORDER</option><option value="1">CAN_ID</option><option value="2">CAN_MASTER</option><option value="3">CAN_TIMEOUT</option><option value="4">M_ZERO</option><option value="5">E_ZERO</option></select><input id="configValue" type="number" step="any" placeholder="配置值"><label class="check"><input id="configCommit" type="checkbox">提交到 Flash</label><button id="configSet">通过 CAN 写入配置</button></p></article></section><p class="panel">',
    1,
)
# The MIT markup is inserted by an earlier chained replacement; tolerate
# either surrounding shape and ensure the trajectory panel is present exactly
# once in the final rendered page.
if '上位机轨迹控制' not in PAGE:
    PAGE = PAGE.replace(
        '</article></section><div class="log-head">',
        '</article>' + _trajectory_panel + '</section><div class="log-head">',
        1,
    )
if '上位机轨迹控制' not in PAGE:
    PAGE = PAGE.replace(
        '</section><div class="log-head">',
        _trajectory_panel + '</section><div class="log-head">',
        1,
    )
if 'id="configSet"' not in PAGE:
    _config_panel = r'''<article class="panel tab-panel" data-tab="readonly"><h2>CAN 配置</h2><p><select id="configField"><option value="3">CAN_TIMEOUT</option><option value="0">PHASE_ORDER</option><option value="1">CAN_ID</option><option value="2">CAN_MASTER</option></select><input id="configValue" type="number" step="any" placeholder="配置值"><label class="check"><input id="configCommit" type="checkbox">提交到 Flash</label><button id="configSet">通过 CAN 写入配置</button></p></article>'''
    PAGE = PAGE.replace(
        '</section><div class="log-head">',
        _config_panel + '</section><div class="log-head">',
        1,
    )
PAGE = PAGE.replace(
    'input{width:100%;',
    'input,select{width:100%;',
    1,
)
PAGE = PAGE.replace(
    "'positionStepStiff','positionStepCustom','mitCustom']",
    "'positionStepStiff','positionStepCustom','mitCustom','trajectoryStart']",
    1,
)
PAGE = PAGE.replace(
    '</body></html>',
    r'''<script>
const trajectoryMode=document.querySelector('#trajectoryMode');
let trajectoryReduction=1;
const outputReductionInput=document.querySelector('#outputReduction'),outputReductionSave=document.querySelector('#outputReductionSave');
if(outputReductionSave)outputReductionSave.onclick=()=>api('/api/output-reduction',{reduction:outputReductionInput.value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));
const mitCoordinate=document.querySelector('#mitCoordinate'),mitOutputInputs=document.querySelector('#mitOutputInputs'),mitOutputP=document.querySelector('#mitOutputP'),mitOutputV=document.querySelector('#mitOutputV'),mitP=document.querySelector('#mitP'),mitV=document.querySelector('#mitV'),mitReductionNote=document.querySelector('#mitReductionNote');
const syncMitCoordinate=()=>{if(!mitCoordinate)return;const output=mitCoordinate.value==='output';mitOutputInputs.hidden=!output;mitP.disabled=output;mitV.disabled=output;if(output){const p=Number(mitOutputP.value),v=Number(mitOutputV.value);if(Number.isFinite(p))mitP.value=(p*trajectoryReduction).toFixed(4);if(Number.isFinite(v))mitV.value=(v*trajectoryReduction).toFixed(4);mitReductionNote.textContent=`按 ${trajectoryReduction}:1 换算，以下电机侧 p/v 为实际发送值`;}else{mitReductionNote.textContent='以下 p/v 直接作为固件 MIT 电机侧命令发送';}};
if(mitCoordinate){mitCoordinate.onchange=syncMitCoordinate;[mitOutputP,mitOutputV].forEach(input=>input.oninput=syncMitCoordinate);syncMitCoordinate();const mitCustom=document.querySelector('#mitCustom');if(mitCustom)mitCustom.onclick=()=>{syncMitCoordinate();api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:mitP.value,velocity:mitV.value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,gravity_torque:document.querySelector('#mitGravity').value,friction_torque:document.querySelector('#mitFriction').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));};}
const trajectoryPreview=(inputId, outputId, label)=>{const input=document.querySelector('#'+inputId),output=document.querySelector('#'+outputId);if(!input||!output)return;const value=Number(input.value);output.textContent=Number.isFinite(value)?`${label}: ${(value*trajectoryReduction).toFixed(4)} rad${inputId.includes('Velocity')||inputId.includes('SpeedLimit')?'/s':''}`:`${label}: 等待输入`;};
const refreshTrajectoryPreviews=()=>{trajectoryPreview('trajectoryTarget','trajectoryTargetMotorPreview','电机侧目标位置');trajectoryPreview('trajectorySpeedLimit','trajectorySpeedLimitMotorPreview','电机侧速度上限');trajectoryPreview('trajectoryVelocity','trajectoryVelocityMotorPreview','电机侧恒速');};
if(trajectoryMode){const syncTrajectoryMode=()=>{const position=trajectoryMode.value==='position',kp=document.querySelector('#trajectoryKp');document.querySelector('#trajectoryPositionInputs').hidden=!position;document.querySelector('#trajectoryVelocityInputs').hidden=position;kp.disabled=!position;if(!position)kp.value='0';refreshTrajectoryPreviews()};trajectoryMode.onchange=syncTrajectoryMode;['trajectoryTarget','trajectorySpeedLimit','trajectoryVelocity'].forEach(id=>document.querySelector('#'+id).oninput=refreshTrajectoryPreviews);syncTrajectoryMode();document.querySelector('#trajectoryStart').onclick=()=>api('/api/bridge/trajectory',{physical_ready:document.querySelector('#enableReady').checked,mode:trajectoryMode.value,target:document.querySelector('#trajectoryTarget').value,speed_limit:document.querySelector('#trajectorySpeedLimit').value,velocity:document.querySelector('#trajectoryVelocity').value,duration:document.querySelector('#trajectoryDuration').value,hold:document.querySelector('#trajectoryHold').value,kp:document.querySelector('#trajectoryKp').value,kd:document.querySelector('#trajectoryKd').value,torque:document.querySelector('#trajectoryTorque').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#trajectoryStop').onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));}
const configSet=document.querySelector('#configSet');
if(configSet)configSet.onclick=()=>api('/api/config/set',{field:document.querySelector('#configField').value,value:document.querySelector('#configValue').value,commit:document.querySelector('#configCommit').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));
</script></body></html>''',
    1,
)
PAGE = PAGE.replace(
    "let s=await r.json();document.querySelector('#sha').textContent=s.normal_sha;",
    "let s=await r.json();if(typeof refreshTrajectoryPreviews==='function'){trajectoryReduction=s.output_reduction||1;if(outputReductionInput&&document.activeElement!==outputReductionInput)outputReductionInput.value=trajectoryReduction;refreshTrajectoryPreviews();if(typeof syncMitCoordinate==='function')syncMitCoordinate()}document.querySelector('#sha').textContent=s.normal_sha;",
    1,
)
PAGE = PAGE.replace(
    '<span>推荐顺序：刷写 → 启动 → PING → Snapshot → DRV 状态 → CAN Trace</span>',
    '',
    1,
)
PAGE = PAGE.replace(
    '<span>当前正常固件 SHA-256</span><code id="shaTop">加载中…</code>',
    '',
    1,
)
PAGE = PAGE.replace(
    "document.querySelector('#shaTop').textContent=s.normal_sha;",
    "const shaTop=document.querySelector('#shaTop');if(shaTop)shaTop.textContent=s.normal_sha;",
    1,
)
PAGE = PAGE.replace(
    '</style>',
    '''.tabs + .summary{margin-top:0;border:1px solid #374151;border-radius:6px;padding:8px 10px;background:#0f172a}.summary{display:flex;align-items:center;gap:6px 10px;flex-wrap:nowrap;overflow-x:auto}.summary span{font-size:11px;color:#94a3b8;white-space:nowrap}.summary code{display:block;white-space:nowrap}.control-row{display:grid!important;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:6px 10px;align-items:end}.tab-panel[data-tab="trajectory"] > p:not(.muted),.tab-panel[data-tab="mit"] > p:not(.muted){display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:6px 10px;align-items:end}.tab-panel output,.tab-panel .muted{align-self:end;padding:7px 0;line-height:1.25}</style>''',
    1,
)

# The rendered handlers send separate load-model terms; the backend composes
# the single signed MIT t_ff field immediately before each frame.
PAGE = PAGE.replace(
    "torque:document.querySelector('#trajectoryTorque').value",
    "gravity_torque:document.querySelector('#trajectoryGravity').value,friction_torque:document.querySelector('#trajectoryFriction').value",
)

# Replace the earlier compatibility markup as complete panels.  Incremental
# substitutions used while the UI evolved made the raw MIT form structurally
# fragile (an input tail could be rendered as text) and mixed unrelated
# controls into one responsive grid.
_mit_panel_final = r'''<article class="panel tab-panel motion-panel" data-tab="mit">
<h2>自定义 MIT 单次命令（电机侧）</h2>
<p class="muted">位置、速度可按输出端填写并自动换算；Kp、Kd、重力与摩擦补偿始终是电机侧 MIT 参数。</p>
<section class="motion-section"><h3>目标坐标</h3>
<div class="control-row"><label>输入坐标系<select id="mitCoordinate"><option value="output">输出端目标（自动换算）</option><option value="motor">电机侧原始 MIT</option></select></label><output id="mitReductionNote" class="muted"></output></div>
<div class="control-row" id="mitOutputInputs"><label>输出端目标位置 (rad)<input id="mitOutputP" type="number" step="0.001" value="0"></label><label>输出端目标速度 (rad/s)<input id="mitOutputV" type="number" step="0.001" value="0"></label></div>
<div class="control-row"><label>电机侧位置 p (rad)<input id="mitP" type="number" step="0.001" value="0"></label><label>电机侧速度 v (rad/s)<input id="mitV" type="number" step="0.001" value="0"></label></div></section>
<section class="motion-section"><h3>MIT 参数</h3>
<div class="control-row"><label>电机侧 MIT Kp<input id="mitKp" type="number" min="0" max="500" step="0.001" value="0"></label><label>电机侧 MIT Kd<input id="mitKd" type="number" min="0" max="5" step="0.001" value="0"></label></div>
<p class="muted">每次点击仅发送 0xFC 使能帧和一帧 MIT 命令，不做分段或重复发送。命令会在固件 CAN_TIMEOUT 后自动失效；需要立即撤销时发送停止帧。</p></section>
<section class="motion-section"><h3>前馈补偿</h3>
<div class="control-row"><label>电机侧重力补偿 (Nm)<input id="mitGravity" type="number" step="0.001" value="0"></label><label>电机侧摩擦补偿幅值 (Nm)<input id="mitFriction" type="number" min="0" step="0.001" value="0"></label></div>
<p class="muted">摩擦补偿指令与期望运动同向；零速保持时按位置误差方向取同向符号。</p></section>
<div class="motion-actions"><button id="mitCustom">发送自定义 MIT</button><button class="danger" id="mitStop">停止 MIT</button></div></article>'''
PAGE = re.sub(r'<article class="panel tab-panel" data-tab="mit">.*?</article>',
              _mit_panel_final, PAGE, count=1, flags=re.S)

_trajectory_panel_final = r'''<article class="panel tab-panel motion-panel" data-tab="trajectory">
<h2>上位机轨迹控制</h2>
<p class="muted">目标位置和速度按减速器输出端填写，页面按当前减速比映射到电机侧；Kp、Kd 与补偿力矩保持电机侧语义。</p>
<section class="motion-section"><h3>映射与模式</h3>
<div class="control-row"><label>减速比（电机转数 / 输出转数）<input id="outputReduction" type="number" min="1" max="1000" step="0.001" value="9"></label><div class="inline-action"><button class="secondary" id="outputReductionSave">保存减速比</button><span class="muted">仅影响上位机 P/V 映射</span></div><label>模式<select id="trajectoryMode" aria-label="轨迹模式"><option value="position">S 曲线到目标位置</option><option value="velocity">恒速前进</option></select></label></div></section>
<section class="motion-section" id="trajectoryPositionInputs"><h3>位置轨迹</h3>
<div class="control-row"><label>输出端目标位置 (rad)<input id="trajectoryTarget" type="number" step="0.001" placeholder="输出端目标位置"></label><output id="trajectoryTargetMotorPreview" class="muted">电机侧目标位置: 等待输入</output><label>输出端速度上限 (rad/s)<input id="trajectorySpeedLimit" type="number" min="0.005" max="7.2" step="0.005" value="0.02"></label><output id="trajectorySpeedLimitMotorPreview" class="muted">电机侧速度上限: 等待输入</output></div></section>
<section class="motion-section" id="trajectoryVelocityInputs" hidden><h3>恒速轨迹</h3>
<div class="control-row"><label>输出端恒速 (rad/s)<input id="trajectoryVelocity" type="number" min="-7.2" max="7.2" step="0.005" value="0.02"></label><output id="trajectoryVelocityMotorPreview" class="muted">电机侧恒速: 等待输入</output></div></section>
<section class="motion-section"><h3>MIT 参数与补偿</h3>
<div class="control-row"><label>执行时间 (s)<input id="trajectoryDuration" type="number" min="0.1" max="300" step="0.1" value="2"></label><label>保持时间 (s)<input id="trajectoryHold" type="number" min="0" max="300" step="0.1" value="2"></label><label>电机侧 MIT Kp<input id="trajectoryKp" type="number" min="0" max="500" step="0.1" value="20"></label><label>电机侧 MIT Kd<input id="trajectoryKd" type="number" min="0" max="5" step="0.01" value="1"></label><label>电机侧重力补偿 (Nm)<input id="trajectoryGravity" type="number" step="0.01" value="0"></label><label>电机侧摩擦补偿幅值 (Nm)<input id="trajectoryFriction" type="number" min="0" step="0.01" value="0"></label></div>
<p class="muted">摩擦补偿指令与期望运动同向；零速保持时根据位置误差方向取同向符号。</p></section>
<div class="motion-actions"><button id="trajectoryStart">开始轨迹</button><button class="danger" id="trajectoryStop">停止轨迹</button></div></article>'''
PAGE = re.sub(r'<article class="panel tab-panel" data-tab="trajectory">.*?</article>',
              _trajectory_panel_final, PAGE, count=1, flags=re.S)
PAGE = PAGE.replace(
    '</style>',
    '''.motion-panel{max-width:none}.motion-section{border-top:1px solid #2b3a50;margin-top:10px;padding-top:9px}.motion-section h3{font-size:12px;font-weight:600;color:#93c5fd;margin:0 0 4px}.motion-panel .control-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:8px 12px;align-items:end}.motion-panel label{display:flex!important;margin:0!important;min-width:0}.motion-panel label input,.motion-panel label select{min-width:0!important;margin:0!important}.motion-panel output{min-height:34px;display:flex;align-items:center;padding:0!important}.inline-action{display:flex;gap:8px;align-items:center;min-height:52px}.motion-actions{display:flex;gap:8px;margin-top:12px}@media(max-width:550px){.motion-panel .control-row{grid-template-columns:1fr}.inline-action{min-height:0;flex-wrap:wrap}}</style>''',
    1,
)

# Diagnostics are sent through the bridge-owned USB session; keep the rendered
# command hints consistent with that single-owner architecture.
PAGE = PAGE.replace(
    'tools/athena_diag_uc12/athena_diag_uc12 ping',
    'CAN 诊断帧 0x701 / PING',
).replace(
    'tools/athena_diag_uc12/athena_diag_uc12 snapshot',
    'CAN 诊断帧 0x701 / Snapshot',
).replace(
    'tools/athena_diag_uc12/athena_diag_uc12 drv-status',
    'CAN 诊断帧 0x701 / DRV 状态',
).replace(
    '读取前请停止 CAN0 Trace。',
    '诊断请求与 MIT 控制共用同一个 CAN0 桥接连接。',
)

# Keep the frequently used CAN controls above the tabs as a compact strip.
_can_start = PAGE.find('<article class="panel can-strip">')
if _can_start >= 0:
    _can_end = PAGE.find('</article>', _can_start) + len('</article>')
    _can_markup = PAGE[_can_start:_can_end]
    PAGE = PAGE[:_can_start] + PAGE[_can_end:]
    PAGE = PAGE.replace('<nav class="tabs"', _can_markup + '<nav class="tabs"', 1)
    PAGE = PAGE.replace(
        '</style>',
        '.can-strip{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:8px 0;padding:8px 10px}.can-strip h2,.can-strip .command,.can-strip .muted,.can-strip hr,.can-strip p:not(:has(button)):not(:has(select)){display:none}.can-strip p{margin:0}.can-strip label.check{margin:0}.can-strip .check input{margin-top:2px}</style>',
        1,
    )

def main() -> None:
    global ACCESS_TOKEN
    parser = argparse.ArgumentParser(description="Athena fixed-action bench web UI")
    parser.add_argument("--host", default="0.0.0.0", help="listen address (default: all LAN interfaces)")
    parser.add_argument("--port", type=int, default=8788)
    parser.add_argument("--token", help="LAN access token; generated when omitted")
    args = parser.parse_args()
    ACCESS_TOKEN = args.token or secrets.token_urlsafe(24)
    RUNNER.log("服务启动；所有 API 动作均需要访问令牌。")
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print("Athena Bench WebUI is listening.")
    print(f"Local: http://127.0.0.1:{args.port}/?token={ACCESS_TOKEN}")
    for address in local_addresses():
        print(f"LAN:   http://{address}:{args.port}/?token={ACCESS_TOKEN}")
    print("Press Ctrl-C to stop. Running actions continue only until their command exits.")
    # Remote deployment restarts this process with SIGTERM.  Convert it to the
    # same controlled path as Ctrl-C so the UC12 bridge releases the USB device
    # instead of surviving as an orphan process.
    def stop_on_signal(_signum: int, _frame: Any) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop_on_signal)
    signal.signal(signal.SIGHUP, stop_on_signal)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        RUNNER.stop_bridge()


if __name__ == "__main__":
    main()
