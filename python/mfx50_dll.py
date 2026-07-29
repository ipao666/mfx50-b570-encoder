from __future__ import annotations

import ctypes
from pathlib import Path

MAX_DEVICES = 4
MAX_PATH = 512


class DeviceRoute(ctypes.Structure):
    _fields_ = [("device_path", ctypes.c_char_p), ("route_count", ctypes.c_int)]


class Config(ctypes.Structure):
    _fields_ = [
        ("route_count", ctypes.c_int),
        ("frames_per_route", ctypes.c_int),
        ("fps_num", ctypes.c_int),
        ("fps_den", ctypes.c_int),
        ("initial_qp", ctypes.c_int),
        ("initial_gop", ctypes.c_int),
        ("async_depth", ctypes.c_int),
        ("device_count", ctypes.c_int),
        ("devices", DeviceRoute * MAX_DEVICES),
        ("write_outputs", ctypes.c_int),
        ("sample_path", ctypes.c_char_p),
        ("enable_internal_roi", ctypes.c_int),
        ("enable_quality_guard", ctypes.c_int),
        ("enable_motion_idr", ctypes.c_int),
    ]


class Stats(ctypes.Structure):
    _fields_ = [
        ("requested_routes", ctypes.c_int),
        ("completed_routes", ctypes.c_int),
        ("routes_below_target_fps", ctypes.c_int),
        ("all_routes_realtime", ctypes.c_int),
        ("frames_per_route", ctypes.c_int),
        ("target_fps", ctypes.c_double),
        ("min_route_fps", ctypes.c_double),
        ("avg_route_fps", ctypes.c_double),
        ("max_route_fps", ctypes.c_double),
        ("common_time_sec", ctypes.c_double),
        ("wall_seconds", ctypes.c_double),
        ("aggregate_fps", ctypes.c_double),
        ("summary_path", ctypes.c_char * MAX_PATH),
        ("par_path", ctypes.c_char * MAX_PATH),
        ("log_path", ctypes.c_char * MAX_PATH),
    ]


class RouteStats(ctypes.Structure):
    _fields_ = [
        ("route_id", ctypes.c_int),
        ("passed", ctypes.c_int),
        ("frames", ctypes.c_int),
        ("seconds", ctypes.c_double),
        ("fps", ctypes.c_double),
        ("device_path", ctypes.c_char * MAX_PATH),
    ]


def _s(buf) -> str:
    return bytes(buf).split(b"\0", 1)[0].decode("utf-8", errors="replace")


class Mfx50:
    def __init__(self, lib_path: str | Path = "./libmfx50_encoder.so") -> None:
        self.lib = ctypes.CDLL(str(lib_path))
        self._bind()
        self.handle = None

    def _bind(self) -> None:
        self.lib.MFX50_DefaultConfig.argtypes = [ctypes.POINTER(Config)]
        self.lib.MFX50_DefaultConfig.restype = ctypes.c_int
        self.lib.MFX50_Create.argtypes = [ctypes.POINTER(Config)]
        self.lib.MFX50_Create.restype = ctypes.c_void_p
        self.lib.MFX50_RunInputList.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        self.lib.MFX50_RunInputList.restype = ctypes.c_int
        self.lib.MFX50_RunSingleInput.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        self.lib.MFX50_RunSingleInput.restype = ctypes.c_int
        self.lib.MFX50_GetStats.argtypes = [ctypes.c_void_p, ctypes.POINTER(Stats)]
        self.lib.MFX50_GetStats.restype = ctypes.c_int
        self.lib.MFX50_GetRouteStats.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(RouteStats)]
        self.lib.MFX50_GetRouteStats.restype = ctypes.c_int
        self.lib.MFX50_GetLastError.argtypes = [ctypes.c_void_p]
        self.lib.MFX50_GetLastError.restype = ctypes.c_char_p
        self.lib.MFX50_Close.argtypes = [ctypes.c_void_p]
        self.lib.MFX50_Close.restype = None

    def default_config(self) -> Config:
        cfg = Config()
        rc = self.lib.MFX50_DefaultConfig(ctypes.byref(cfg))
        if rc != 0:
            raise RuntimeError("MFX50_DefaultConfig failed")
        return cfg

    def create(self, cfg: Config) -> None:
        self.handle = self.lib.MFX50_Create(ctypes.byref(cfg))
        if not self.handle:
            raise RuntimeError(self.last_error(None))

    def run_input_list(self, input_list: str | Path, output_dir: str | Path) -> int:
        return int(
            self.lib.MFX50_RunInputList(
                self.handle,
                str(input_list).encode("utf-8"),
                str(output_dir).encode("utf-8"),
            )
        )

    def run_single_input(self, input_path: str | Path, output_dir: str | Path) -> int:
        return int(
            self.lib.MFX50_RunSingleInput(
                self.handle,
                str(input_path).encode("utf-8"),
                str(output_dir).encode("utf-8"),
            )
        )

    def stats(self) -> Stats:
        st = Stats()
        self.lib.MFX50_GetStats(self.handle, ctypes.byref(st))
        return st

    def route_stats(self, route_id: int) -> RouteStats:
        st = RouteStats()
        self.lib.MFX50_GetRouteStats(self.handle, int(route_id), ctypes.byref(st))
        return st

    def last_error(self, handle=None) -> str:
        raw = self.lib.MFX50_GetLastError(self.handle if handle is None else handle)
        return raw.decode("utf-8", errors="replace") if raw else ""

    def close(self) -> None:
        if self.handle:
            self.lib.MFX50_Close(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def c_string(buf) -> str:
    return _s(buf)
