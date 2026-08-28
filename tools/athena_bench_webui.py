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
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from athena_mit_codec import DEFAULT_RANGES, decode_feedback, encode_command, format_slcan


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

MIT_CHECK_FRAME = "t00187FFF7FF0000007FF\r"
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

def _semantic_can_line(clean: str) -> str | None:
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
            torque = tq * 80.0 / 4095.0 - 40.0
            if abs(torque) <= 0.01:
                torque = 0.0
            return ("CAN 语义 TX: MIT p={:.4f} rad, v={:.3f} rad/s, "
                    "Kp={:.2f}, Kd={:.3f}, t_ff={:.3f} Nm".format(
                        p * 25.0 / 65535.0 - 12.5,
                        v * 130.0 / 4095.0 - 65.0,
                        kp * 500.0 / 4095.0,
                        kd * 5.0 / 4095.0,
                        torque))
    rx = FEEDBACK_RE.search(clean)
    if rx:
        decoded = decode_feedback(bytes.fromhex(rx.group(1)))
        torque = float(decoded["torque"])
        if abs(torque) <= 0.01:
            torque = 0.0
        decoded["torque"] = torque
        return ("CAN 语义 RX: 反馈(上一控制周期) p={position:.4f} rad, "
                "v_est={velocity:.3f} rad/s, t_filt={torque:.3f} Nm"
                .format(**decoded))
    return None


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
        self.logical_position_rad: float | None = None
        self._logical_feedback_time: float | None = None
        self._logical_tracking_active = False
        self.feedback_generation = 0
        self.diag_sequence = 0

    def log(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        for line in text.rstrip("\n").splitlines() or [""]:
            self.logs.append(f"[{stamp}] {line}")

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
            return True, "配置已提交；涉及 CAN/外设初始化的参数需重启生效"
        return True, "配置已暂存并请求校验；点击提交后才写入 Flash"

    def send_diag_action(self, name: str) -> tuple[bool, str]:
        if name == "ping":
            requests = [(0x00, 0, 0)]
        elif name == "snapshot":
            requests = [(0x02, page, 0) for page in
                        (0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19)]
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
            if not self.enable_active:
                return False, "当前没有正在运行的 MIT 会话"
            self.mit_stop_event.set()
            tty = self.bridge_tty
        self.log("已请求停止 MIT 持续会话；等待当前帧写入结束")
        if tty:
            try:
                with self._open_serial(tty) as serial_port:
                    serial_port.write(STOP_FRAME)
                self.log("MIT 停止帧已发送: ID=0x001 data=FF FF FF FF FF FF FF FD")
            except (OSError, TimeoutError) as exc:
                self.log(f"MIT 停止帧发送失败: {exc}")
        return True, "已请求停止 MIT 会话"

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
        return True, "MIT 持续会话已启动"

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
        try:
            mode = str(values.get("mode", "position"))
            kp = float(values.get("kp"))
            kd = float(values.get("kd"))
            torque = float(values.get("torque", 0.0))
            duration = float(values.get("duration"))
            hold = float(values.get("hold", 0.0))
        except (TypeError, ValueError):
            return False, "轨迹参数必须填写数字"
        if mode not in ("position", "velocity"):
            return False, "未知轨迹模式"
        for name, value, low, high in (
                ("Kp", kp, 0.0, 500.0), ("Kd", kd, 0.0, 5.0),
                ("前馈力矩", torque, -40.0, 40.0),
                ("执行时间", duration, 0.1, MIT_MAX_DURATION_S),
                ("保持时间", hold, 0.0, MIT_MAX_DURATION_S)):
            if not math.isfinite(value) or not low <= value <= high:
                return False, f"{name} 超出允许范围 {low:g}..{high:g}"
        try:
            if mode == "position":
                target = float(values.get("target"))
                speed_limit = float(values.get("speed_limit"))
                if not DEFAULT_RANGES.position_min <= target <= DEFAULT_RANGES.position_max or not math.isfinite(target):
                    return False, ("目标位置超出当前 MIT 位置范围 "
                                   f"{DEFAULT_RANGES.position_min:g}..{DEFAULT_RANGES.position_max:g} rad")
                if not 0.05 <= speed_limit <= TRAJECTORY_MAX_SPEED_RAD_S or not math.isfinite(speed_limit):
                    return False, f"速度上限必须在 0.05..{TRAJECTORY_MAX_SPEED_RAD_S:g} rad/s"
            else:
                velocity = float(values.get("velocity"))
                if not 0.05 <= abs(velocity) <= TRAJECTORY_MAX_SPEED_RAD_S or not math.isfinite(velocity):
                    return False, f"速度必须在 +/-0.05..{TRAJECTORY_MAX_SPEED_RAD_S:g} rad/s"
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
            if mode == "position":
                minimum_duration = 1.5 * abs(target - start) / speed_limit
                if duration < minimum_duration:
                    return False, (f"执行时间过短：该 S 曲线至少需要 {minimum_duration:.3f} 秒，"
                                   f"才能不超过 {speed_limit:g} rad/s")
            else:
                # Velocity mode is continuous.  The encoded MIT position is
                # rebased around live feedback on every frame; logical multi-
                # turn position is tracked independently on the host.
                target = start
            self.enable_active = True
            self.mit_stop_event.clear()
            self.mit_session_frames = 0
            if mode == "velocity":
                # The MIT feedback position is finite, while the velocity
                # feedback remains meaningful after it reaches an endpoint.
                # Rebase the host-owned multi-turn coordinate at each session.
                self.logical_position_rad = start
                self._logical_feedback_time = time.monotonic()
                self._logical_tracking_active = True
            request = {"mode": mode, "kp": kp, "kd": kd, "torque": torque,
                       "duration": duration, "hold": hold, "start": start,
                       "target": target}
            if mode == "position":
                request["speed_limit"] = speed_limit
            else:
                request["velocity"] = velocity
            self.mit_thread = threading.Thread(
                target=self._trajectory_session_worker, args=(request,), daemon=True)
            self.mit_thread.start()
        return True, "上位机轨迹会话已启动"

    def _trajectory_session_worker(self, request: dict[str, float | str]) -> None:
        mode = str(request["mode"])
        start = float(request["start"])
        target = float(request["target"])
        duration = float(request["duration"])
        hold = float(request["hold"])
        kp, kd, torque = (float(request[name]) for name in ("kp", "kd", "torque"))
        with self.lock:
            tty = self.bridge_tty
        try:
            if not tty:
                self.log("轨迹会话取消：CAN0 Trace 已停止")
                return
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log("S 曲线轨迹: " if mode == "position" else "恒速轨迹: ")
                self.log(f"起点={start:.4f} rad, 终点={target:.4f} rad, 执行={duration:g} s, "
                         f"Kp={kp:g}, Kd={kd:g}, t_ff={torque:g}")
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
                        position = start if feedback_position is None else feedback_position
                        position = min(DEFAULT_RANGES.position_max,
                                       max(DEFAULT_RANGES.position_min, position))
                    now = time.monotonic()
                    if now < next_send:
                        time.sleep(next_send - now)
                    now = time.monotonic()
                    serial_port.write(format_slcan(1, encode_command(position, velocity, kp, kd, torque)))
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
                                             else self.last_feedback_position_rad)
                        hold_position = min(DEFAULT_RANGES.position_max,
                                            max(DEFAULT_RANGES.position_min, hold_position))
                    serial_port.write(format_slcan(1, encode_command(hold_position, 0.0, kp, kd, torque)))
                    self.mit_session_frames += 1
                    next_send += MIT_KEEPALIVE_INTERVAL_S
                    if next_send < now:
                        next_send = now + MIT_KEEPALIVE_INTERVAL_S
                serial_port.write(STOP_FRAME)
                self.log("轨迹会话已发送停止帧（0xFD）")
                self.log(f"轨迹发送统计：{self.mit_session_frames} 帧，最大帧间隔 {max_gap * 1000:.1f} ms")
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
            semantic = _semantic_can_line(clean)
            if semantic and not QUIET_TRACE:
                self.log("BRIDGE " + semantic)
            match = FEEDBACK_RE.search(clean)
            if match:
                payload = bytes.fromhex(match.group(1))
                if len(payload) == 6:
                    decoded = decode_feedback(payload)
                    now = time.monotonic()
                    with self.lock:
                        self.last_feedback_position = payload[1:3].hex().upper()
                        self.last_feedback_position_rad = float(decoded["position"])
                        self.last_feedback_velocity_rad_s = float(decoded["velocity"])
                        if self.logical_position_rad is None:
                            self.logical_position_rad = self.last_feedback_position_rad
                        elif self._logical_tracking_active and self._logical_feedback_time is not None:
                            dt = min(0.25, max(0.0, now - self._logical_feedback_time))
                            self.logical_position_rad += self.last_feedback_velocity_rad_s * dt
                        self._logical_feedback_time = now
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
        if not live or not tty:
            return False, "请先启动 CAN0 trace，并等待 Serial Port 路径出现"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(MIT_CHECK_FRAME)
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
                    serial_port.write(MIT_CHECK_FRAME)
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

    def hold_zero_once(self) -> tuple[bool, str]:
        """Enable, then send bounded zero-output MIT keepalives at last position."""
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证以获得位置反馈"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        zero_frame = f"t0018{position}7FF0000007FF\r"
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        # Position is copied from feedback. v=0, Kp ~= 1/500 full scale,
        # Kd=0 and feed-forward torque=0. No position step is commanded.
        tiny_frame = f"t0018{position}7FF0080007FF\r"
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        # t=0x800 is one quantization step around zero with +/-40 Nm range
        # (about +0.02 Nm). Position, velocity, Kp and Kd remain neutral.
        torque_frame = f"t0018{position}7FF000000800\r"
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        # t=0x809 is approximately +0.2 Nm with the +/-40 Nm range.
        torque_frame = f"t0018{position}7FF000000809\r"
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        # t=0x819 is approximately +0.5 Nm with the +/-40 Nm range.
        torque_frame = f"t0018{position}7FF000000819\r"
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        # t=0x832 is approximately +1.0 Nm with the +/-40 Nm range.
        torque_frame = f"t0018{position}7FF000000832\r"
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        current = int(position, 16)
        target = (current + 46) & 0xFFFF  # ~1 degree over the +/-12.5 rad range
        target_hex = f"{target:04X}"
        # v=0, Kp ~= 5/500 full scale, Kd=0, feed-forward torque=0.
        position_frame = f"t0018{target_hex}7FF0{0x29:02X}0007FF\r"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"1°位置阶跃: {position} -> {target_hex}，低 Kp，零前馈力矩")
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
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        current = int(position, 16)
        target_hex = f"{(current + 46) & 0xFFFF:04X}"
        # v=0, Kp ~= 20/500 full scale (0xA4), Kd=0, torque=0.
        position_frame = f"t0018{target_hex}7FF0A40007FF\r"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"1°位置阶跃(Kp≈20): {position} -> {target_hex}，零前馈力矩")
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
        if not 0.0 <= kp <= 100.0:
            return False, "Kp 必须在 0 到 100 之间"
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            position = self.last_feedback_position
            if self.mit_check_active or self.enable_active:
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace，并先完成一次 MIT 验证"
            if len(position) != 4:
                return False, "尚未获得控制板位置反馈；请先发送三次 MIT 非使能验证"
            self.enable_active = True
        target_hex = f"{(int(position, 16) + 46) & 0xFFFF:04X}"
        kp_raw = min(4095, max(0, int(round(kp * 4095.0 / 500.0))))
        position_frame = f"t0018{target_hex}7FF0{(kp_raw >> 8) & 0x0F:02X}{kp_raw & 0xFF:02X}07FF\r"
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"自定义 Kp 位置阶跃: Kp={kp:g}, {position} -> {target_hex}，零前馈力矩")
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
        """Send an explicitly entered MIT command for a bounded session."""
        names = ("position", "velocity", "kp", "kd", "torque")
        limits = ((-12.5, 12.5), (-65.0, 65.0), (0.0, 500.0), (0.0, 5.0), (-40.0, 40.0))
        try:
            parsed = [float(values.get(name)) for name in names]
        except (TypeError, ValueError):
            return False, "MIT 五个参数都必须填写数字"
        for name, value, (low, high) in zip(names, parsed, limits):
            if not math.isfinite(value) or not low <= value <= high:
                return False, f"{name} 超出允许范围 {low}..{high}"
        try:
            duration = float(values.get("duration", MIT_DEFAULT_DURATION_S))
        except (TypeError, ValueError):
            return False, "持续时间必须是数字"
        if not math.isfinite(duration) or not 0.1 <= duration <= MIT_MAX_DURATION_S:
            return False, f"持续时间必须在 0.1..{MIT_MAX_DURATION_S:g} 秒之间"
        with self.lock:
            tty = self.bridge_tty
            live = self.bridge is not None and self.bridge.poll() is None
            if self.mit_check_active or (self.enable_active and not values.get("_worker")):
                return False, "已有 CAN 测试或使能动作运行中"
            if not live or not tty:
                return False, "请先启动 CAN0 trace"
            # start_mit_session owns the session flag; this method is the worker.
        data = encode_command(*parsed)
        frame = format_slcan(1, data)
        try:
            with self._open_serial(tty) as serial_port:
                serial_port.write(ENABLE_FRAME)
                self.log(f"自定义 MIT: p={parsed[0]:g} rad, v={parsed[1]:g} rad/s, Kp={parsed[2]:g}, Kd={parsed[3]:g}, t={parsed[4]:g} Nm")
                deadline = time.monotonic() + duration
                next_send = time.monotonic()
                last_send = None
                max_gap = 0.0
                while time.monotonic() < deadline and not self.mit_stop_event.is_set():
                    now = time.monotonic()
                    if now < next_send:
                        time.sleep(next_send - now)
                    now = time.monotonic()
                    serial_port.write(frame)
                    if last_send is not None:
                        max_gap = max(max_gap, now - last_send)
                    last_send = now
                    self.mit_session_frames += 1
                    next_send += MIT_KEEPALIVE_INTERVAL_S
                    if next_send < now:
                        next_send = now + MIT_KEEPALIVE_INTERVAL_S
                # End every bounded session explicitly.  Letting the stream
                # go silent makes the firmware watchdog perform the normal
                # stop, but leaves the next enable racing a stale DRV/encoder
                # fault.  0xFD follows the firmware's clean ESC path.
                serial_port.write(STOP_FRAME)
                self.log("自定义 MIT 会话已发送停止帧（0xFD）")
                self.log(f"MIT 发送统计：{self.mit_session_frames} 帧，最大帧间隔 {max_gap * 1000:.1f} ms")
        except OSError as exc:
            return False, f"无法写入桥接伪串口 {tty}: {exc}"
        finally:
            with self.lock:
                self.enable_active = False
        stopped = self.mit_stop_event.is_set()
        self.log("自定义 MIT 会话结束；" + ("用户已停止" if stopped else "达到设定时长"))
        return True, ("MIT 会话已停止" if stopped else f"已完成 {duration:g} 秒自定义 MIT 会话")

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
            "last_feedback_position_rad": self.last_feedback_position_rad,
            "last_feedback_velocity_rad_s": self.last_feedback_velocity_rad_s,
            "logical_position_rad": self.logical_position_rad,
            "logical_turns": None if self.logical_position_rad is None else self.logical_position_rad / (2.0 * math.pi),
            "normal_sha": normal_firmware_config()[0],
            "normal_image": str(normal_firmware_config()[1]),
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
        "make", "SAFE_BRINGUP=0", "BRINGUP_INJECT=0",
        f"BUILD_DIR={REPO / 'build/webui-normal'}", f"GCC_PATH={TOOLCHAIN}", "-j4",
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
<article class="panel"><h2>离线验证</h2><p>构建前或代码修改后执行。不会访问控制板。</p><div class="command">make host-test host-app-test host-tools-test</div><p><button data-action="offline-tests">执行离线主机测试</button></p><div class="command">make SAFE_BRINGUP=0 BRINGUP_INJECT=0 BUILD_DIR=build/webui-normal GCC_PATH=&lt;持久化 Arm GNU Toolchain&gt; -j4</div><p><button data-action="build-normal">重新构建正常固件</button></p></article>
<article class="panel"><h2>刷写与启动</h2><p>刷写使用逐页擦写、写入、读回校验，并保留 CPU halted。启动前不发送任何运动命令。</p><div class="command">tools/athena_safe_flash.sh flash-normal --confirm-normal-sha <span id="sha"></span> --i-understand-this-writes-main-flash</div><input id="shaInput" aria-label="SHA-256" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="可手动粘贴完整 SHA-256"><p><button class="secondary" id="fillSha" type="button">填入当前 SHA</button></p><label class="check"><input id="physical" type="checkbox">我已确认控制板、ST-LINK、限流电源、机械固定和可断电路径均已就绪。</label><button class="danger" id="flash">刷入正常固件</button><hr><div class="command">tools/athena_safe_flash.sh boot-normal</div><label class="check"><input id="bootReady" type="checkbox">我已确认物理台架可安全启动。</label><button id="boot">启动正常固件</button></article>
<article class="panel"><h2>正常固件只读验证</h2><p>PING、Snapshot 和 DRV 状态均为只读，不会启用电机。</p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 ping</div><p><button data-action="diag-ping">执行 PING</button></p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 snapshot</div><p><button data-action="diag-snapshot">执行 Snapshot</button></p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 drv-status</div><p><button data-action="diag-drv-status">读取 DRV 状态</button> <button class="secondary" data-action="diag-drv-fault-snapshot">读取故障瞬间快照</button></p><hr><h2>RAM 调试日志</h2><p>默认关闭；开启后只记录运行期关键事件，重启会清空。读取前请停止 CAN0 Trace。</p><p><button data-action="diag-debug-on">开启记录</button> <button class="secondary" data-action="diag-debug-status">读取状态</button> <button class="secondary" data-action="diag-debug-off">关闭记录</button></p><div style="display:flex;gap:6px;align-items:center"><input id="debugLogIndex" type="number" min="0" max="31" step="1" value="0" aria-label="RAM 调试日志索引"><button id="debugLogRead" class="secondary">读取该条日志</button></div></article>
<article class="panel"><h2>CAN0 收发与受限使能</h2><p>桥接独占 UC12。先用非使能帧确认收到控制板回复，再在电机固定、限流和断电路径确认后发送一次 0xFC。</p><div class="command">./uc12_slcan_bridge --channel 0 --unsafe-tx --trace</div><p><button id="bridgeStart">启动 CAN0 Trace</button> <button class="secondary" id="bridgeStop">停止</button></p><div class="command">printf 't00187FFF7FF0000007FF\\r' &gt; &lt;bridge-pty&gt; (固定执行 3 次，间隔 200 ms)</div><p><button id="mitCheck">发送三次 MIT 非使能验证</button></p><div class="command">printf 't0018FFFFFFFFFFFFFFFC\\r' &gt; &lt;bridge-pty&gt; (仅发送一次)</div><label class="check"><input id="enableReady" type="checkbox">我已确认电机已固定、限流已设置，并能立即断电。</label><p><button class="danger" id="enableOnce">单次受限使能</button> <button id="holdZero">1 秒零输出保持</button> <button id="holdTinyKp">1 秒极小 Kp 闭环</button> <button id="feedforwardMin">1 秒约 0.02 Nm</button> <button id="feedforwardLow">1 秒约 0.2 Nm</button> <button id="feedforwardMedium">1 秒约 0.5 Nm</button> <button id="feedforwardHigh">1 秒约 1.0 Nm</button> <button id="positionStep">1 秒 1° 位置阶跃</button> <button id="positionStepStiff">1 秒 1° 阶跃(Kp≈20)</button></p><p class="muted">力矩测试持续 1 秒自动停止。位置阶跃会尝试小幅运动，必须确认运动空间已释放、限流 0.2 A 且可立即断电。</p></article>
</section><p class="panel"><label>Kp（位置阶跃，0-100）：<input id="customKp" type="number" min="0" max="100" step="1" value="20"></label> <button id="positionStepCustom">执行输入 Kp 的 1° 阶跃</button></p><div class="log-head"><h2>实时日志</h2><button class="secondary" id="clearLogs">清除当前内容</button></div><pre id="log">等待认证…</pre></main><script>
const params=new URLSearchParams(location.search), fromUrl=params.get('token'); let token=fromUrl||localStorage.getItem('athenaBenchToken')||'';if(fromUrl)localStorage.setItem('athenaBenchToken',fromUrl);if(!token){token=prompt('输入服务启动时显示的访问令牌：')||'';localStorage.setItem('athenaBenchToken',token)}
const note=t=>document.querySelector('#notice').textContent=t;const api=async(path,body)=>{let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Bench-Token':token},body:JSON.stringify(body||{})});let j=await r.json();if(!r.ok)throw Error(j.error||j.message||r.status);return j};
async function action(name,extra={}){try{let j=await api('/api/action',{action:name,...extra});note(j.message)}catch(e){note('失败: '+e.message)}}
document.querySelector('#positionStepCustom').onclick=()=>api('/api/bridge/position-step-custom',{physical_ready:document.querySelector('#enableReady').checked,kp:document.querySelector('#customKp').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#debugLogRead').onclick=()=>api('/api/debug/log',{index:document.querySelector('#debugLogIndex').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));
document.querySelectorAll('[data-action]').forEach(b=>b.onclick=()=>action(b.dataset.action));document.querySelector('#fillSha').onclick=()=>{document.querySelector('#shaInput').value=document.querySelector('#sha').textContent;note('已填入当前镜像 SHA-256')};document.querySelector('#flash').onclick=()=>action('flash-normal',{physical_ready:document.querySelector('#physical').checked,sha:document.querySelector('#shaInput').value.trim()});document.querySelector('#boot').onclick=()=>action('boot-normal',{physical_ready:document.querySelector('#bootReady').checked});document.querySelector('#bridgeStart').onclick=()=>api('/api/bridge/start').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#bridgeStop').onclick=()=>api('/api/bridge/stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitCheck').onclick=()=>api('/api/bridge/mit-check').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#enableOnce').onclick=()=>api('/api/bridge/enable-once',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#holdZero').onclick=()=>api('/api/bridge/hold-zero',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#holdTinyKp').onclick=()=>api('/api/bridge/hold-tiny-kp',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardMin').onclick=()=>api('/api/bridge/feedforward-min',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardLow').onclick=()=>api('/api/bridge/feedforward-low',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardMedium').onclick=()=>api('/api/bridge/feedforward-medium',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#feedforwardHigh').onclick=()=>api('/api/bridge/feedforward-high',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#positionStep').onclick=()=>api('/api/bridge/position-step',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#positionStepStiff').onclick=()=>api('/api/bridge/position-step-stiff',{physical_ready:document.querySelector('#enableReady').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#clearLogs').onclick=()=>api('/api/logs/clear').then(x=>{document.querySelector('#log').textContent='';note(x.message)}).catch(e=>note('失败: '+e.message));
async function refresh(){try{let r=await fetch('/api/status',{headers:{'X-Bench-Token':token}});if(!r.ok)throw Error('令牌无效');let s=await r.json();document.querySelector('#sha').textContent=s.normal_sha;document.querySelector('#shaTop').textContent=s.normal_sha;document.querySelector('#positionRad').textContent=s.last_feedback_position_rad===null?'暂无':Number(s.last_feedback_position_rad).toFixed(5)+' rad';let lp=document.querySelector('#logicalPosition'),lt=document.querySelector('#logicalTurns');if(lp)lp.textContent=s.logical_position_rad===null?'暂无':Number(s.logical_position_rad).toFixed(5)+' rad';if(lt)lt.textContent=s.logical_turns===null?'暂无':Number(s.logical_turns).toFixed(4);let busy=s.mit_active||s.enable_active;document.querySelector('#state').textContent=s.active?'正在执行: '+s.active.name:(busy?'CAN 动作运行中':(s.bridge_running?'CAN0 Trace 运行中 '+s.bridge_tty:'空闲'));['mitCheck','enableOnce','holdZero','holdTinyKp','feedforwardMin','feedforwardLow','feedforwardMedium','feedforwardHigh','positionStep','positionStepStiff','positionStepCustom','mitCustom'].forEach(id=>{let e=document.querySelector('#'+id);if(e)e.disabled=busy});let log=document.querySelector('#log'),nearEnd=log.scrollHeight-log.scrollTop-log.clientHeight<40;log.textContent=s.logs.join('\n');if(nearEnd)log.scrollTop=log.scrollHeight}catch(e){note('无法读取状态: '+e.message)}}refresh();setInterval(refresh,1200);
</script></body></html>'''


PAGE = PAGE.replace(
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
    '<style>', '<style>.tabs{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0}.tabs .tab{background:#172033;color:#cbd5e1}.tabs .tab.active{background:#2563eb;color:#fff}.tab-panel[hidden]{display:none}.can-strip{grid-column:1/-1}.can-strip p{display:flex;gap:6px;flex-wrap:wrap;align-items:center}.can-strip select{max-width:240px;width:auto;background:#0b1220;border:1px solid #475569;color:#e5e7eb;border-radius:4px;padding:7px}.legacy-control{display:none!important}',
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
    "document.querySelector('#mitCustom').onclick=()=>api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:document.querySelector('#mitP').value,velocity:document.querySelector('#mitV').value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,torque:document.querySelector('#mitT').value,duration:document.querySelector('#mitDuration').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitStop').onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelectorAll('[data-action]').forEach",
).replace(
    '<span>当前正常固件 SHA-256</span>',
    '<span>当前正常固件 SHA-256</span>',
).replace(
    "document.querySelector('#shaTop').textContent=s.normal_sha;",
    "document.querySelector('#shaTop').textContent=s.normal_sha;",
)
PAGE = PAGE.replace(
    "document.querySelector('#mitCustom').onclick=()=>api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:document.querySelector('#mitP').value,velocity:document.querySelector('#mitV').value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,torque:document.querySelector('#mitT').value,duration:document.querySelector('#mitDuration').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitStop').onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));",
    "const mitCustomButton=document.querySelector('#mitCustom'),mitStopButton=document.querySelector('#mitStop');if(mitCustomButton)mitCustomButton.onclick=()=>api('/api/bridge/mit-custom',{physical_ready:document.querySelector('#enableReady').checked,position:document.querySelector('#mitP').value,velocity:document.querySelector('#mitV').value,kp:document.querySelector('#mitKp').value,kd:document.querySelector('#mitKd').value,torque:document.querySelector('#mitT').value,duration:document.querySelector('#mitDuration').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));if(mitStopButton)mitStopButton.onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));",
    1,
)

# The fixed Kp position-step control is intentionally no longer exposed in the UI.
# Keep its backend endpoint for compatibility with older scripts, but remove its
# panel and click binding from the rendered page.
PAGE = re.sub(r'<p class="panel"><label>Kp（位置阶跃，0-100）：.*?</p>', '', PAGE, flags=re.S)
PAGE = re.sub(r"document\.querySelector\('#positionStepCustom'\)\.onclick=.*?;\n", '', PAGE)

# Upper-layer motion planning is deliberately separate from the raw five
# parameter MIT panel.  The raw panel remains useful for protocol/FOC checks;
# this panel owns the time-varying position and velocity references needed for
# repeatable motion tests.
_trajectory_panel = r'''<article class="panel tab-panel" data-tab="trajectory"><h2>上位机轨迹控制</h2><p>位置模式以 20 ms 周期生成三次 S 曲线的 p(t)/v(t)。恒速模式固定 Kp=0，只用 Kd 速度环；MIT 位置字段保持在可编码范围内，主机按反馈速度记录逻辑多圈位置与圈数。</p><p><select id="trajectoryMode" aria-label="轨迹模式"><option value="position">S 曲线到目标位置</option><option value="velocity">恒速前进</option></select></p><p id="trajectoryPositionInputs"><input id="trajectoryTarget" type="number" step="0.001" placeholder="目标位置 rad (-100..100)"><input id="trajectorySpeedLimit" type="number" min="0.05" max="20" step="0.05" value="2" placeholder="最大速度 rad/s"></p><p id="trajectoryVelocityInputs" hidden><input id="trajectoryVelocity" type="number" min="-20" max="20" step="0.05" value="0.2" placeholder="速度 rad/s（正负决定方向）"></p><p><input id="trajectoryDuration" type="number" min="0.1" max="300" step="0.1" value="2" placeholder="执行时间 s"><input id="trajectoryHold" type="number" min="0" max="300" step="0.1" value="2" placeholder="到达后保持 s（0 为不保持）"><input id="trajectoryKp" type="number" min="0" max="500" step="0.1" value="20" placeholder="Kp"><input id="trajectoryKd" type="number" min="0" max="5" step="0.01" value="1" placeholder="Kd"><input id="trajectoryTorque" type="number" min="-40" max="40" step="0.01" value="0" placeholder="前馈力矩 Nm"></p><p class="muted">位置模式受当前 P_MIN/P_MAX 限制。恒速模式开始时以最新反馈重置逻辑位置，随后由反馈速度积分；停止后不保持转动。</p><button id="trajectoryStart">开始轨迹</button> <button class="danger" id="trajectoryStop">停止轨迹</button></article>'''
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
if(trajectoryMode){const syncTrajectoryMode=()=>{const position=trajectoryMode.value==='position',kp=document.querySelector('#trajectoryKp');document.querySelector('#trajectoryPositionInputs').hidden=!position;document.querySelector('#trajectoryVelocityInputs').hidden=position;kp.disabled=!position;if(!position)kp.value='0'};trajectoryMode.onchange=syncTrajectoryMode;syncTrajectoryMode();document.querySelector('#trajectoryStart').onclick=()=>api('/api/bridge/trajectory',{physical_ready:document.querySelector('#enableReady').checked,mode:trajectoryMode.value,target:document.querySelector('#trajectoryTarget').value,speed_limit:document.querySelector('#trajectorySpeedLimit').value,velocity:document.querySelector('#trajectoryVelocity').value,duration:document.querySelector('#trajectoryDuration').value,hold:document.querySelector('#trajectoryHold').value,kp:document.querySelector('#trajectoryKp').value,kd:document.querySelector('#trajectoryKd').value,torque:document.querySelector('#trajectoryTorque').value}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#trajectoryStop').onclick=()=>api('/api/bridge/mit-stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));}
const configSet=document.querySelector('#configSet');
if(configSet)configSet.onclick=()=>api('/api/config/set',{field:document.querySelector('#configField').value,value:document.querySelector('#configValue').value,commit:document.querySelector('#configCommit').checked}).then(x=>note(x.message)).catch(e=>note('失败: '+e.message));
</script></body></html>''',
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
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        RUNNER.stop_bridge()


if __name__ == "__main__":
    main()
