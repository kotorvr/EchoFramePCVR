# Runs on the Steam Frame (python3, stdlib only): talks to the Steam client through its
# DevTools port (Steam runs with -cef-enable-debugging) to manage Echo's shortcut.
# The DevTools client is EchoFrame's patcher/frame_setup.py STEAM_PY (MIT, Copyright (c)
# 2026 heisthecat31), itself after Frame Control's frame/android/steam_shortcuts.py (MIT,
# Copyright (c) 2026 saphid).
#
#   steam_cdp.py add NAME EXE START_DIR      -> the new shortcut's app id
#   steam_cdp.py has APPID                   -> yes / no
#   steam_cdp.py configure APPID TOOL OPTS   -> forces compat tool TOOL, sets launch options, VR flag
#   steam_cdp.py eval JS                     -> whatever JS returns (debugging)
import base64, json, os, socket, struct, sys, urllib.request


def target_ws():
    for t in json.load(urllib.request.urlopen("http://127.0.0.1:8080/json", timeout=5)):
        if t.get("title") == "SharedJSContext":
            return t["webSocketDebuggerUrl"]
    sys.exit("SharedJSContext not found: is the Steam client running?")


class WS:
    def __init__(self, url, timeout=20):
        host_port, path = url[len("ws://"):].split("/", 1)
        host, port = host_port.split(":")
        self.s = socket.create_connection((host, int(port)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\n"
                        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        if b" 101 " not in buf.split(b"\r\n", 1)[0]:
            sys.exit("websocket handshake failed")
        self.rest = buf.split(b"\r\n\r\n", 1)[1]

    def _read(self, n):
        while len(self.rest) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.rest += chunk
        out, self.rest = self.rest[:n], self.rest[n:]
        return out

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else
                                bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else
                                bytes([0x80 | 127]) + struct.pack(">Q", n))
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        msg = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            msg += self._read(n)
            if b0 & 0x80:
                return msg.decode()


def evaluate(js, timeout=20):
    ws = WS(target_ws(), timeout)
    ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {
        "expression": js, "awaitPromise": True, "returnByValue": True}}))
    while True:
        r = json.loads(ws.recv())
        if r.get("id") == 1:
            break
    res = r.get("result", {})
    if "exceptionDetails" in res:
        sys.exit("JS error: " + json.dumps(res["exceptionDetails"])[:500])
    return res.get("result", {}).get("value")


cmd, args = sys.argv[1], sys.argv[2:]
q = json.dumps
if cmd == "add":
    name, exe, start = args[:3]
    print(evaluate(f"""(async () => {{
      const id = await SteamClient.Apps.AddShortcut({q(name)}, {q(exe)}, "", "");
      SteamClient.Apps.SetShortcutName(id, {q(name)});
      SteamClient.Apps.SetShortcutStartDir(id, {q(start)});
      return id;
    }})()"""))
elif cmd == "has":
    print("yes" if evaluate(f"!!appStore.GetAppOverviewByAppID({int(args[0])})") else "no")
elif cmd == "configure":
    appid, tool, options = int(args[0]), args[1], args[2]
    print(evaluate(f"""(async () => {{
      SteamClient.Apps.SpecifyCompatTool({appid}, {q(tool)});
      SteamClient.Apps.SetShortcutLaunchOptions({appid}, {q(options)});
      if (typeof SteamClient.Apps.SetShortcutIsVR === "function") SteamClient.Apps.SetShortcutIsVR({appid}, true);
      return "ok";
    }})()"""))
elif cmd == "eval":
    print(json.dumps(evaluate(args[0])))
else:
    sys.exit("unknown command " + cmd)
