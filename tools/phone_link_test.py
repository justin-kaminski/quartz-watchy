#!/usr/bin/env python3
"""Drive the watch's Bluetooth phone link from a Linux PC (BlueZ), like the companion page does.

Bring-up helper for docs/HARDWARE_BRINGUP.md B13 and agents: pairs with passkey entry, runs
console commands over the link, checks USB-only refusals, then disconnects.

  .venv/bin/python tools/phone_link_test.py [--passkey-file PATH] [--name-prefix Quartz]

Start "Sync with phone" on the watch first (menu, or over USB with qzctl `btn` commands). When the
watch asks for pairing, the six digits shown on the watch are read from --passkey-file (write the
file once they appear: an agent can read them from `qzctl screenshot`); with no file argument they
are read from stdin. Requires `pip install bleak` (dbus-fast comes with it).
"""
from __future__ import annotations

import argparse
import asyncio
import os
import sys
import time

from bleak import BleakClient, BleakScanner
from dbus_fast import BusType
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method

RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
AGENT_PATH = "/quartz/agent"
CHUNK = 20


class Agent(ServiceInterface):
    """BlueZ pairing agent with KeyboardOnly capability: the watch displays, we type."""

    def __init__(self, passkey_file: str | None):
        super().__init__("org.bluez.Agent1")
        self.passkey_file = passkey_file
        self.asked = False

    def _read_passkey(self) -> int:
        self.asked = True
        if self.passkey_file is None:
            return int(input("passkey shown on the watch: ").strip())
        print(f"waiting for the passkey in {self.passkey_file}", flush=True)
        deadline = time.time() + 120
        while time.time() < deadline:
            if os.path.exists(self.passkey_file):
                text = open(self.passkey_file).read().strip()
                if text.isdigit():
                    return int(text)
            time.sleep(0.5)
        raise RuntimeError("no passkey")

    @method()
    def Release(self):  # noqa: N802
        pass

    @method()
    def RequestPasskey(self, device: "o") -> "u":  # noqa: F821,N802
        return self._read_passkey()

    @method()
    def DisplayPasskey(self, device: "o", passkey: "u", entered: "q"):  # noqa: F821,N802
        pass

    @method()
    def RequestConfirmation(self, device: "o", passkey: "u"):  # noqa: F821,N802
        pass

    @method()
    def RequestAuthorization(self, device: "o"):  # noqa: F821,N802
        pass

    @method()
    def AuthorizeService(self, device: "o", uuid: "s"):  # noqa: F821,N802
        pass

    @method()
    def Cancel(self):  # noqa: N802
        print("pairing cancelled by BlueZ", flush=True)


async def register_agent(agent: Agent) -> MessageBus:
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    bus.export(AGENT_PATH, agent)
    intro = await bus.introspect("org.bluez", "/org/bluez")
    mgr = bus.get_proxy_object("org.bluez", "/org/bluez", intro).get_interface(
        "org.bluez.AgentManager1")
    await mgr.call_register_agent(AGENT_PATH, "KeyboardOnly")
    await mgr.call_request_default_agent(AGENT_PATH)
    return bus


class Link:
    def __init__(self, client: BleakClient):
        self.client = client
        self.buffer = ""
        self.lines: asyncio.Queue[str] = asyncio.Queue()
        self.next_id = 1

    def on_notify(self, _sender, data: bytearray):
        self.buffer += data.decode("utf-8", "replace")
        while "\n" in self.buffer:
            line, self.buffer = self.buffer.split("\n", 1)
            self.lines.put_nowait(line)

    async def run(self, command: str, timeout: float = 15.0) -> str:
        rid = f"t{self.next_id}"
        self.next_id += 1
        data = f"#{rid} {command}\n".encode()
        for i in range(0, len(data), CHUNK):
            await self.client.write_gatt_char(RX, data[i:i + CHUNK], response=True)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = await asyncio.wait_for(self.lines.get(), deadline - time.time())
            except asyncio.TimeoutError:
                break
            if line.startswith(f"@QZ1 {rid} "):
                return line
        raise TimeoutError(f"no answer to {command!r}")


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--passkey-file")
    ap.add_argument("--name-prefix", default="Quartz")
    ap.add_argument("--scan-timeout", type=float, default=30.0)
    args = ap.parse_args()

    agent = Agent(args.passkey_file)
    bus = await register_agent(agent)
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith(args.name_prefix),
        timeout=args.scan_timeout)
    if device is None:
        print("no watch advertising: start Sync with phone on the watch", file=sys.stderr)
        return 2
    print(f"found {device.name} {device.address}", flush=True)
    failures = 0
    async with BleakClient(device) as client:
        link = Link(client)
        # The watch requests security on connect; retry until the protected link is up.
        t0 = time.time()
        while True:
            try:
                await client.start_notify(TX, link.on_notify)
                first = await link.run("version", timeout=5)
                break
            except Exception as e:  # noqa: BLE001 - BlueZ surfaces auth errors as generic ones
                if time.time() - t0 > 90:
                    print(f"link never became secure: {e}", file=sys.stderr)
                    return 1
                await asyncio.sleep(1.5)
        print(f"secure after {time.time() - t0:.1f} s (passkey asked: {agent.asked})")
        print(first)
        checks = [
            ("status", " OK "),
            ("settings get units", " OK "),
            ("reboot", " ERR unsupported "),
            ("factory-reset confirm", " ERR unsupported "),
            ("display crc", " OK "),
        ]
        for command, expect in checks:
            line = await link.run(command)
            ok = expect in line
            failures += 0 if ok else 1
            print(("PASS " if ok else "FAIL ") + command + " -> " + line[:160])
        t1 = time.time()
        big = await link.run("display dump", timeout=60)
        print(f"display dump: {len(big)} bytes in {time.time() - t1:.1f} s")
        failures += 0 if " OK " in big and len(big) > 6000 else 1
    bus.disconnect()
    print("disconnected; failures:", failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
