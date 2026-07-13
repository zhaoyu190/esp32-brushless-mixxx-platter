#!/usr/bin/env python3
import argparse
import ctypes
import time


class MidiMonitor:
    def __init__(self, source_name: str) -> None:
        self.cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self.cm = ctypes.CDLL("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI")

        self.cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
        self.cf.CFStringCreateWithCString.restype = ctypes.c_void_p
        self.cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
        self.cf.CFStringGetCString.restype = ctypes.c_bool
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]
        self.cf.CFRunLoopRunInMode.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_bool]
        self.cf.CFRunLoopRunInMode.restype = ctypes.c_int32
        self.default_run_loop_mode = ctypes.c_void_p.in_dll(self.cf, "kCFRunLoopDefaultMode")

        self.cm.MIDIClientCreate.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.cm.MIDIClientCreate.restype = ctypes.c_int32
        self.cm.MIDIInputPortCreate.argtypes = [
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.cm.MIDIInputPortCreate.restype = ctypes.c_int32
        self.cm.MIDIGetNumberOfSources.argtypes = []
        self.cm.MIDIGetNumberOfSources.restype = ctypes.c_uint32
        self.cm.MIDIGetSource.argtypes = [ctypes.c_uint32]
        self.cm.MIDIGetSource.restype = ctypes.c_uint32
        self.cm.MIDIObjectGetStringProperty.argtypes = [
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.cm.MIDIObjectGetStringProperty.restype = ctypes.c_int32
        self.cm.MIDIPortConnectSource.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        self.cm.MIDIPortConnectSource.restype = ctypes.c_int32

        self.source_name = source_name
        self.client = ctypes.c_uint32(0)
        self.port = ctypes.c_uint32(0)
        self.callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
        self.callback = self.callback_type(self._read_packets)

        client_name = self._cf_string("ESP32 MIDI Monitor")
        port_name = self._cf_string("ESP32 MIDI Monitor In")
        self._check(self.cm.MIDIClientCreate(client_name, None, None, ctypes.byref(self.client)), "MIDIClientCreate")
        self._check(
            self.cm.MIDIInputPortCreate(self.client, port_name, self.callback, None, ctypes.byref(self.port)),
            "MIDIInputPortCreate",
        )
        self.cf.CFRelease(client_name)
        self.cf.CFRelease(port_name)

    def _cf_string(self, value: str) -> ctypes.c_void_p:
        return self.cf.CFStringCreateWithCString(None, value.encode("utf-8"), 0x08000100)

    def _string_property(self, endpoint: int, prop: str) -> str:
        prop_name = self._cf_string(prop)
        cf_value = ctypes.c_void_p()
        status = self.cm.MIDIObjectGetStringProperty(endpoint, prop_name, ctypes.byref(cf_value))
        self.cf.CFRelease(prop_name)
        if status != 0 or not cf_value.value:
            return ""
        buf = ctypes.create_string_buffer(512)
        ok = self.cf.CFStringGetCString(cf_value, buf, len(buf), 0x08000100)
        self.cf.CFRelease(cf_value)
        return buf.value.decode("utf-8", "replace") if ok else ""

    @staticmethod
    def _check(status: int, name: str) -> None:
        if status != 0:
            raise RuntimeError(f"{name} failed with OSStatus {status}")

    def connect(self) -> None:
        count = self.cm.MIDIGetNumberOfSources()
        names = []
        for index in range(count):
            endpoint = self.cm.MIDIGetSource(index)
            name = self._string_property(endpoint, "name")
            names.append(name)
            if name == self.source_name:
                self._check(self.cm.MIDIPortConnectSource(self.port, endpoint, None), "MIDIPortConnectSource")
                print(f"monitoring MIDI source: {name}")
                return
        raise RuntimeError(f"source {self.source_name!r} not found; available: {names}")

    def run(self, seconds: float) -> None:
        deadline = time.time() + seconds if seconds > 0 else None
        while deadline is None or time.time() < deadline:
            self.cf.CFRunLoopRunInMode(self.default_run_loop_mode, 0.1, False)

    def _read_packets(self, packet_list_ptr: int, _read_ref: int, _src_ref: int) -> None:
        packet_count = ctypes.c_uint32.from_address(packet_list_ptr).value
        packet_ptr = packet_list_ptr + 8
        for _ in range(packet_count):
            length = ctypes.c_uint16.from_address(packet_ptr + 8).value
            data_addr = packet_ptr + 10
            data = [ctypes.c_ubyte.from_address(data_addr + i).value for i in range(length)]
            self._print_message(data)
            packet_ptr = data_addr + ((length + 3) & ~3)

    @staticmethod
    def _print_message(data: list[int]) -> None:
        if len(data) >= 3 and data[0] & 0xF0 == 0xB0:
            channel = (data[0] & 0x0F) + 1
            print(f"cc channel={channel} controller={data[1]} value={data[2]} raw={data}", flush=True)
        else:
            print(f"midi raw={data}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Monitor a CoreMIDI source by name.")
    parser.add_argument("--source-name", default="ESP32 Brushless Platter")
    parser.add_argument("--seconds", type=float, default=0.0, help="Stop after this many seconds; 0 means forever.")
    args = parser.parse_args()

    monitor = MidiMonitor(args.source_name)
    monitor.connect()
    monitor.run(args.seconds)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
