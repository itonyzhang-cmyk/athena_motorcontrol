#!/usr/bin/env python3
"""Small authenticated LAN panel for the reviewed Athena bench workflow.

Only named workflow actions below can execute. There is deliberately no shell
command input endpoint.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import shlex
import socket
import subprocess
import threading
import time
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


REPO = Path(__file__).resolve().parent.parent
WORKSPACE = REPO.parent
TOOLCHAIN = Path("/tmp/arm-gnu-toolchain-15.2-root-new/bin")
NORMAL_SHA = "7b4539af52a3d845d31b3488405a3f594ac14ddde23830b89f4604a020dc13fc"
NORMAL_BIN = REPO / "artifacts/athena_normal_channel_gate_20260822/motorcontrol.bin"
BRIDGE = WORKSPACE / "tools/uc12_slcan_bridge/uc12_slcan_bridge"
DIAG = REPO / "tools/athena_diag_uc12/athena_diag_uc12"
FLASH = REPO / "tools/athena_safe_flash.sh"

MIT_CHECK_FRAME = "t00187FFF7FF0000007FF\r"


class Runner:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.active: dict[str, Any] | None = None
        self.logs: deque[str] = deque(maxlen=1600)
        self.last_result: dict[str, Any] = {"state": "idle", "exit_code": None}
        self.bridge: subprocess.Popen[str] | None = None
        self.bridge_tty = ""
        self.mit_check_active = False

    def log(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        for line in text.rstrip("\n").splitlines() or [""]:
            self.logs.append(f"[{stamp}] {line}")

    def clear_logs(self) -> None:
        with self.lock:
            self.logs.clear()

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
                [str(BRIDGE), "--channel", "0", "--unsafe-tx", "--trace"],
                cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
            )
            thread = threading.Thread(target=self._read_bridge, daemon=True)
            thread.start()
        self.log("$ " + shlex.join([str(BRIDGE), "--channel", "0", "--unsafe-tx", "--trace"]))
        return True, "CAN0 trace 桥接已启动，等待伪串口路径"

    def _read_bridge(self) -> None:
        assert self.bridge is not None and self.bridge.stdout is not None
        process = self.bridge
        for line in process.stdout:
            clean = line.rstrip("\n")
            self.log("BRIDGE " + clean)
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
            return False, "桥接未在 3 秒内退出；请勿启动新桥接，先停止占用进程"
        return True, "CAN0 trace 桥接已停止并释放 UC12"

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
            self.mit_check_active = True
        if not live or not tty:
            with self.lock:
                self.mit_check_active = False
            return False, "请先启动 CAN0 trace 桥接，并等待 Serial Port 路径出现"
        try:
            with open(tty, "w", encoding="ascii", buffering=1) as serial_port:
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

    def status(self) -> dict[str, Any]:
        with self.lock:
            bridge_running = self.bridge is not None and self.bridge.poll() is None
            active = dict(self.active) if self.active else None
        return {
            "active": active,
            "last_result": self.last_result,
            "bridge_running": bridge_running,
            "bridge_tty": self.bridge_tty,
            "normal_sha": NORMAL_SHA,
            "normal_image": str(NORMAL_BIN),
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
        "BUILD_DIR=/tmp/athena-normal-webui", f"GCC_PATH={TOOLCHAIN}", "-j4",
    ]
    return {
        "offline-tests": ("离线主机测试", ["make", "host-test", "host-app-test", "host-tools-test"], False),
        "build-normal": ("重新构建正常固件", build, False),
        "flash-normal": (
            "刷入正常固件",
            [str(FLASH), "flash-normal", "--confirm-normal-sha", NORMAL_SHA,
             "--i-understand-this-writes-main-flash"], True,
        ),
        "boot-normal": ("启动正常固件", [str(FLASH), "boot-normal"], True),
        "diag-ping": ("正常固件兼容 PING", [str(DIAG), "ping"], False),
        "diag-snapshot": ("诊断 Snapshot", [str(DIAG), "snapshot"], False),
        "diag-drv-status": ("正常固件 DRV 状态", [str(DIAG), "drv-status"], False),
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
        if parsed.path == "/api/action":
            action = str(body.get("action", ""))
            entry = actions().get(action)
            if entry is None:
                self._json(HTTPStatus.BAD_REQUEST, {"error": "unknown action"})
                return
            label, command, requires_confirmation = entry
            if requires_confirmation:
                if not body.get("physical_ready"):
                    self._json(HTTPStatus.BAD_REQUEST, {"error": "请确认台架、限流和断电路径已就绪"})
                    return
                if action == "flash-normal" and body.get("sha") != NORMAL_SHA:
                    self._json(HTTPStatus.BAD_REQUEST, {"error": "SHA-256 未匹配当前镜像"})
                    return
            ok, message = RUNNER.start_action(label, command)
            self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})
            return
        if parsed.path == "/api/bridge/start":
            ok, message = RUNNER.start_bridge()
        elif parsed.path == "/api/bridge/stop":
            ok, message = RUNNER.stop_bridge()
        elif parsed.path == "/api/bridge/mit-check":
            ok, message = RUNNER.mit_check_repeat()
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
<p id="notice"></p><div class="summary"><span>当前正常固件 SHA-256</span><code id="shaTop">加载中…</code><span>推荐顺序：刷写 → 启动 → PING → Snapshot → DRV 状态 → CAN Trace</span></div><section class="grid">
<article class="panel"><h2>离线验证</h2><p>构建前或代码修改后执行。不会访问控制板。</p><div class="command">make host-test host-app-test host-tools-test</div><p><button data-action="offline-tests">执行离线主机测试</button></p><div class="command">make SAFE_BRINGUP=0 BRINGUP_INJECT=0 BUILD_DIR=/tmp/athena-normal-webui GCC_PATH=/tmp/arm-gnu-toolchain-15.2-root-new/bin -j4</div><p><button data-action="build-normal">重新构建正常固件</button></p></article>
<article class="panel"><h2>刷写与启动</h2><p>刷写使用逐页擦写、写入、读回校验，并保留 CPU halted。启动前不发送任何运动命令。</p><div class="command">tools/athena_safe_flash.sh flash-normal --confirm-normal-sha <span id="sha"></span> --i-understand-this-writes-main-flash</div><input id="shaInput" aria-label="SHA-256" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="可手动粘贴完整 SHA-256"><p><button class="secondary" id="fillSha" type="button">填入当前 SHA</button></p><label class="check"><input id="physical" type="checkbox">我已确认控制板、ST-LINK、限流电源、机械固定和可断电路径均已就绪。</label><button class="danger" id="flash">刷入正常固件</button><hr><div class="command">tools/athena_safe_flash.sh boot-normal</div><label class="check"><input id="bootReady" type="checkbox">我已确认物理台架可安全启动。</label><button id="boot">启动正常固件</button></article>
<article class="panel"><h2>正常固件只读验证</h2><p>PING、Snapshot 和 DRV 状态均为只读，不会启用电机。</p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 ping</div><p><button data-action="diag-ping">执行 PING</button></p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 snapshot</div><p><button data-action="diag-snapshot">执行 Snapshot</button></p><div class="command">tools/athena_diag_uc12/athena_diag_uc12 drv-status</div><p><button data-action="diag-drv-status">读取 DRV 状态</button></p></article>
<article class="panel"><h2>CAN0 收发证据</h2><p>桥接独占 UC12。按钮固定发送 3 次 `0x001`、DLC 8 的非使能帧，间隔 200 ms，不含 `0xFC`；通过条件是日志出现 3 条控制板的 `TRACE CAN RX t000#...`，且电机无动作。</p><div class="command">./uc12_slcan_bridge --channel 0 --unsafe-tx --trace</div><p><button id="bridgeStart">启动 CAN0 Trace</button> <button class="secondary" id="bridgeStop">停止</button></p><div class="command">printf 't00187FFF7FF0000007FF\\r' &gt; &lt;bridge-pty&gt; (固定执行 3 次，间隔 200 ms)</div><p><button id="mitCheck">发送三次 MIT 非使能验证</button></p></article>
</section><div class="log-head"><h2>实时日志</h2><button class="secondary" id="clearLogs">清除当前内容</button></div><pre id="log">等待认证…</pre></main><script>
const params=new URLSearchParams(location.search), fromUrl=params.get('token'); let token=fromUrl||localStorage.getItem('athenaBenchToken')||'';if(fromUrl)localStorage.setItem('athenaBenchToken',fromUrl);if(!token){token=prompt('输入服务启动时显示的访问令牌：')||'';localStorage.setItem('athenaBenchToken',token)}
const note=t=>document.querySelector('#notice').textContent=t;const api=async(path,body)=>{let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Bench-Token':token},body:JSON.stringify(body||{})});let j=await r.json();if(!r.ok)throw Error(j.error||j.message||r.status);return j};
async function action(name,extra={}){try{let j=await api('/api/action',{action:name,...extra});note(j.message)}catch(e){note('失败: '+e.message)}}
document.querySelectorAll('[data-action]').forEach(b=>b.onclick=()=>action(b.dataset.action));document.querySelector('#fillSha').onclick=()=>{document.querySelector('#shaInput').value=document.querySelector('#sha').textContent;note('已填入当前镜像 SHA-256')};document.querySelector('#flash').onclick=()=>action('flash-normal',{physical_ready:document.querySelector('#physical').checked,sha:document.querySelector('#shaInput').value.trim()});document.querySelector('#boot').onclick=()=>action('boot-normal',{physical_ready:document.querySelector('#bootReady').checked});document.querySelector('#bridgeStart').onclick=()=>api('/api/bridge/start').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#bridgeStop').onclick=()=>api('/api/bridge/stop').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#mitCheck').onclick=()=>api('/api/bridge/mit-check').then(x=>note(x.message)).catch(e=>note('失败: '+e.message));document.querySelector('#clearLogs').onclick=()=>api('/api/logs/clear').then(x=>{document.querySelector('#log').textContent='';note(x.message)}).catch(e=>note('失败: '+e.message));
async function refresh(){try{let r=await fetch('/api/status',{headers:{'X-Bench-Token':token}});if(!r.ok)throw Error('令牌无效');let s=await r.json();document.querySelector('#sha').textContent=s.normal_sha;document.querySelector('#shaTop').textContent=s.normal_sha;document.querySelector('#state').textContent=s.active?'正在执行: '+s.active.name:(s.bridge_running?'CAN0 Trace 运行中 '+s.bridge_tty:'空闲');let log=document.querySelector('#log'),nearEnd=log.scrollHeight-log.scrollTop-log.clientHeight<40;log.textContent=s.logs.join('\n');if(nearEnd)log.scrollTop=log.scrollHeight}catch(e){note('无法读取状态: '+e.message)}}refresh();setInterval(refresh,1200);
</script></body></html>'''


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
