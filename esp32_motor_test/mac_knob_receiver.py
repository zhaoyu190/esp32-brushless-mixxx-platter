#!/usr/bin/env python3
import argparse
import ctypes
import json
import socket
import subprocess
import sys
import time


class CoreMidiOut:
    def __init__(self, source_name: str) -> None:
        self.core_foundation = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self.core_midi = ctypes.CDLL("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI")

        self.core_foundation.CFStringCreateWithCString.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_uint32,
        ]
        self.core_foundation.CFStringCreateWithCString.restype = ctypes.c_void_p
        self.core_foundation.CFRelease.argtypes = [ctypes.c_void_p]
        self.core_foundation.CFRunLoopRunInMode.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_bool]
        self.core_foundation.CFRunLoopRunInMode.restype = ctypes.c_int32
        self.default_run_loop_mode = ctypes.c_void_p.in_dll(self.core_foundation, "kCFRunLoopDefaultMode")

        self.core_midi.MIDIClientCreate.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.core_midi.MIDIClientCreate.restype = ctypes.c_int32
        self.core_midi.MIDISourceCreate.argtypes = [
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.core_midi.MIDISourceCreate.restype = ctypes.c_int32
        self.core_midi.MIDIDestinationCreate.argtypes = [
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.core_midi.MIDIDestinationCreate.restype = ctypes.c_int32
        self.core_midi.MIDIReceived.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        self.core_midi.MIDIReceived.restype = ctypes.c_int32
        self.core_midi.MIDIPacketListInit.argtypes = [ctypes.c_void_p]
        self.core_midi.MIDIPacketListInit.restype = ctypes.c_void_p
        self.core_midi.MIDIPacketListAdd.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self.core_midi.MIDIPacketListAdd.restype = ctypes.c_void_p

        client_name = self._cf_string("ESP32 DJ Controller")
        midi_source_name = self._cf_string(source_name)
        self.client = ctypes.c_uint32(0)
        self.source = ctypes.c_uint32(0)
        self.destination = ctypes.c_uint32(0)
        self.on_cc = None
        self.callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
        self.callback = self.callback_type(self._read_packets)
        self._check(self.core_midi.MIDIClientCreate(client_name, None, None, ctypes.byref(self.client)), "MIDIClientCreate")
        self._check(self.core_midi.MIDISourceCreate(self.client, midi_source_name, ctypes.byref(self.source)), "MIDISourceCreate")
        self._check(
            self.core_midi.MIDIDestinationCreate(
                self.client,
                midi_source_name,
                self.callback,
                None,
                ctypes.byref(self.destination),
            ),
            "MIDIDestinationCreate",
        )
        self.core_foundation.CFRelease(client_name)
        self.core_foundation.CFRelease(midi_source_name)

    def _cf_string(self, value: str) -> ctypes.c_void_p:
        k_cf_string_encoding_utf8 = 0x08000100
        return self.core_foundation.CFStringCreateWithCString(None, value.encode("utf-8"), k_cf_string_encoding_utf8)

    @staticmethod
    def _check(status: int, name: str) -> None:
        if status != 0:
            raise RuntimeError(f"{name} failed with OSStatus {status}")

    def send(self, data: list[int]) -> None:
        packet_list = ctypes.create_string_buffer(1024)
        packet = self.core_midi.MIDIPacketListInit(ctypes.byref(packet_list))
        midi_bytes = (ctypes.c_ubyte * len(data))(*[byte & 0xFF for byte in data])
        packet = self.core_midi.MIDIPacketListAdd(
            ctypes.byref(packet_list),
            ctypes.sizeof(packet_list),
            packet,
            0,
            len(data),
            ctypes.byref(midi_bytes),
        )
        if not packet:
            raise RuntimeError("MIDIPacketListAdd failed")
        self._check(self.core_midi.MIDIReceived(self.source, ctypes.byref(packet_list)), "MIDIReceived")

    def cc(self, controller: int, value: int, channel: int) -> None:
        status = 0xB0 | ((channel - 1) & 0x0F)
        self.send([status, controller & 0x7F, value & 0x7F])

    def poll(self, seconds: float = 0.001) -> None:
        self.core_foundation.CFRunLoopRunInMode(self.default_run_loop_mode, seconds, False)

    def _read_packets(self, packet_list_ptr: int, _read_ref: int, _src_ref: int) -> None:
        packet_count = ctypes.c_uint32.from_address(packet_list_ptr).value
        packet_ptr = packet_list_ptr + 4
        for _ in range(packet_count):
            length = ctypes.c_uint16.from_address(packet_ptr + 8).value
            data_addr = packet_ptr + 10
            data = [ctypes.c_ubyte.from_address(data_addr + i).value for i in range(length)]
            self._handle_input(data)
            packet_ptr = data_addr + ((length + 3) & ~3)

    def _handle_input(self, data: list[int]) -> None:
        if len(data) >= 3 and data[0] & 0xF0 == 0xB0:
            channel = (data[0] & 0x0F) + 1
            controller = data[1]
            value = data[2]
            print(f"midi in cc channel={channel} controller={controller} value={value}")
            if self.on_cc:
                self.on_cc(channel, controller, value)
        else:
            print(f"midi in raw={data}")


def run_osascript(script: str) -> str:
    return subprocess.check_output(["osascript", "-e", script], text=True).strip()


def change_volume(delta: int, step: int) -> None:
    current = int(run_osascript("output volume of (get volume settings)"))
    target = max(0, min(100, current + delta * step))
    subprocess.run(["osascript", "-e", f"set volume output volume {target}"], check=False)
    print(f"volume {current} -> {target}")


def scroll(delta: int, step: int) -> None:
    try:
        import Quartz  # type: ignore

        event = Quartz.CGEventCreateScrollWheelEvent(None, Quartz.kCGScrollEventUnitLine, 1, delta * step)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, event)
        print(f"scroll {delta * step}")
    except Exception:
        key_code = 126 if delta > 0 else 125
        for _ in range(abs(delta) * max(1, step // 3)):
            subprocess.run(
                ["osascript", "-e", f'tell application "System Events" to key code {key_code}'],
                check=False,
            )
        print(f"scroll fallback {delta}")


def send_relative_jog(midi: CoreMidiOut, delta: int, jog_cc: int, channel: int) -> None:
    # Common "relative 2's-complement around 64" mapping: 65..72 forward, 63..56 backward.
    step = max(-8, min(8, delta))
    if step > 0:
        value = 64 + step
    elif step < 0:
        value = 64 + step
    else:
        return
    midi.cc(jog_cc, value, channel)
    print(f"midi jog delta={step} cc={jog_cc} value={value}")


def send_touch(midi: CoreMidiOut, touched: int, touch_cc: int, channel: int) -> None:
    value = 127 if touched else 0
    midi.cc(touch_cc, value, channel)
    print(f"midi touch value={value} cc={touch_cc}")


def send_motor_velocity(sock: socket.socket, host: str, port: int, velocity: float) -> None:
    payload = json.dumps({"type": "motor", "velocity": velocity}, separators=(",", ":")).encode("utf-8")
    sock.sendto(payload, (host, port))
    print(f"motor velocity={velocity:.3f} -> {host}:{port}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Receive ESP32 haptic knob UDP events on macOS.")
    parser.add_argument("--port", type=int, default=4210)
    parser.add_argument("--volume-step", type=int, default=4)
    parser.add_argument("--scroll-step", type=int, default=4)
    parser.add_argument("--midi", action="store_true", help="Create a CoreMIDI virtual source for djay.")
    parser.add_argument("--midi-source-name", default="ESP32 Brushless Platter")
    parser.add_argument("--midi-channel", type=int, default=1)
    parser.add_argument("--jog-cc", type=int, default=16)
    parser.add_argument("--touch-cc", type=int, default=17)
    parser.add_argument("--mixxx-play-cc", type=int, default=20)
    parser.add_argument("--esp32-host", default="")
    parser.add_argument("--esp32-port", type=int, default=4210)
    parser.add_argument("--platter-velocity", type=float, default=6.28318530718)
    args = parser.parse_args()

    midi = CoreMidiOut(args.midi_source_name) if args.midi else None
    if midi:
        print(
            f"MIDI source '{args.midi_source_name}' ready: "
            f"channel={args.midi_channel}, jog CC={args.jog_cc}, touch CC={args.touch_cc}"
        )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(0.05)
    sock.bind(("0.0.0.0", args.port))
    print(f"listening on UDP :{args.port}")

    if midi and args.esp32_host:
        motor_state = {"velocity": 0.0, "last_send": 0.0}

        def set_motor_velocity(velocity: float) -> None:
            motor_state["velocity"] = velocity
            motor_state["last_send"] = time.monotonic()
            send_motor_velocity(sock, args.esp32_host, args.esp32_port, velocity)

        def handle_cc(channel: int, controller: int, value: int) -> None:
            if channel != args.midi_channel:
                return
            if controller == args.mixxx_play_cc:
                velocity = args.platter_velocity if value >= 64 else 0.0
                set_motor_velocity(velocity)

        midi.on_cc = handle_cc
    else:
        motor_state = None

    while True:
        if midi:
            midi.poll(0.001)
        if motor_state and abs(motor_state["velocity"]) > 0.001 and time.monotonic() - motor_state["last_send"] > 0.5:
            motor_state["last_send"] = time.monotonic()
            send_motor_velocity(sock, args.esp32_host, args.esp32_port, motor_state["velocity"])
        try:
            data, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue
        try:
            event = json.loads(data.decode("utf-8"))
        except json.JSONDecodeError:
            print(f"bad packet from {addr}: {data!r}")
            continue

        event_type = event.get("type")
        delta = int(event.get("delta", 0))
        value = int(event.get("value", 0))
        print(f"{addr[0]} {event_type} delta={delta} value={value}")
        if event_type == "volume":
            if not delta:
                continue
            change_volume(delta, args.volume_step)
        elif event_type == "scroll":
            if not delta:
                continue
            scroll(delta, args.scroll_step)
        elif event_type == "midi_jog" and midi:
            send_relative_jog(midi, delta, args.jog_cc, args.midi_channel)
        elif event_type == "midi_platter" and midi:
            send_relative_jog(midi, delta, args.jog_cc, args.midi_channel)
        elif event_type == "midi_touch" and midi:
            send_touch(midi, value, args.touch_cc, args.midi_channel)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nstopped")
        raise SystemExit(0)
