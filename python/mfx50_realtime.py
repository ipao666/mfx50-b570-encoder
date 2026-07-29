from __future__ import annotations

import ctypes
from pathlib import Path
from typing import Callable, Optional


MFX50_CODEC_H264 = 1
MFX50_CODEC_HEVC = 2

MFX50_INPUT_ENCODED_PACKET = 1
MFX50_INPUT_RAW_FRAME = 2

MFX50_OUTPUT_POLL = 1
MFX50_OUTPUT_CALLBACK = 2

MFX50RT_DROP_NONE = 0
MFX50RT_DROP_OLDEST = 1
MFX50RT_DROP_NON_KEY_UNTIL_IDR = 2

MFX50_PROFILE_THROUGHPUT_ONLY = 0
MFX50_PROFILE_QUALITY_90_NEAR = 1
MFX50_PROFILE_COMPRESS_85_PROBE = 2
MFX50_PROFILE_DEBUG_TRACE = 3
MFX50_PROFILE_COMPRESS_90_PROBE_A = 4
MFX50_PROFILE_COMPRESS_90_PROBE_B = 5
MFX50_PROFILE_COMPRESS_90_PROBE_C = 6
MFX50_PROFILE_COMPRESS_90_PROBE_D = 7

MFX50RT_ALGO_FLAG_PREPROCESS = 0x00000001
MFX50RT_ALGO_FLAG_SMOOTH_SCALE = 0x00000002
MFX50RT_ALGO_FLAG_PRE_DENOISE = 0x00000004
MFX50RT_ALGO_FLAG_SCENE_ANALYZER = 0x00000008
MFX50RT_ALGO_FLAG_ADAPTIVE_PROFILE = 0x00000010
MFX50RT_ALGO_FLAG_ADAPTIVE_QP = 0x00000020
MFX50RT_ALGO_FLAG_MBQP = 0x00000040

MFX50RT_MBQP_DISABLED_NONE = 0
MFX50RT_MBQP_DISABLED_UNSUPPORTED = 1
MFX50RT_MBQP_DISABLED_NOT_PROBED = 2
MFX50RT_MBQP_DISABLED_RUNTIME_FAILED = 3

MFX50RT_API_VERSION = 1

MFX50_OK = 0
MFX50_ERR_INVALID_ARG = -1
MFX50_ERR_DEVICE = -2
MFX50_ERR_DECODE = -3
MFX50_ERR_ENCODE = -4
MFX50_ERR_NOT_IMPLEMENTED = -5
MFX50_ERR_BACKPRESSURE = -44
MFX50_ERR_BUFFER_TOO_SMALL = -43
MFX50_ERR_NO_OUTPUT = 1
MFX50_ERR_NEED_MORE_INPUT = 2
MFX50_ERR_AGAIN = 3


class MFX50RTConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("input_mode", ctypes.c_int),
        ("output_mode", ctypes.c_int),
        ("input_codec", ctypes.c_int),
        ("output_codec", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("fps_num", ctypes.c_int),
        ("fps_den", ctypes.c_int),
        ("device_selector", ctypes.c_char_p),
        ("profile", ctypes.c_int),
        ("route_count", ctypes.c_int),
        ("async_depth", ctypes.c_int),
        ("max_queue_packets", ctypes.c_int),
        ("max_queue_surfaces", ctypes.c_int),
        ("algo_budget_us", ctypes.c_int),
        ("target_usage", ctypes.c_int),
        ("gop", ctypes.c_int),
        ("gop_ref_dist", ctypes.c_int),
        ("num_ref_frame", ctypes.c_int),
        ("qpi", ctypes.c_int),
        ("qpp", ctypes.c_int),
        ("qpb", ctypes.c_int),
        ("bref_type", ctypes.c_int),
        ("enable_trace", ctypes.c_int),
        ("trace_path", ctypes.c_char_p),
        ("user_opaque", ctypes.c_void_p),
        ("abi_version", ctypes.c_uint32),
        ("async_mode", ctypes.c_int),
        ("max_input_queue_packets", ctypes.c_int),
        ("max_output_queue_packets", ctypes.c_int),
        ("drop_policy", ctypes.c_int),
    ]


class MFX50RTPacket(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("stream_id", ctypes.c_int),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_size_t),
        ("pts", ctypes.c_int64),
        ("dts", ctypes.c_int64),
        ("is_keyframe", ctypes.c_int),
        ("end_of_stream", ctypes.c_int),
        ("user_opaque", ctypes.c_void_p),
    ]


class MFX50RTEncodedPacket(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("stream_id", ctypes.c_int),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_size_t),
        ("capacity", ctypes.c_size_t),
        ("pts", ctypes.c_int64),
        ("dts", ctypes.c_int64),
        ("is_keyframe", ctypes.c_int),
        ("frame_type", ctypes.c_int),
        ("user_opaque", ctypes.c_void_p),
    ]


class MFX50RTStats(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("input_packets", ctypes.c_uint64),
        ("decoded_frames", ctypes.c_uint64),
        ("encoded_frames", ctypes.c_uint64),
        ("output_packets", ctypes.c_uint64),
        ("dropped_frames", ctypes.c_uint64),
        ("fallback_frames", ctypes.c_uint64),
        ("fps_in", ctypes.c_double),
        ("fps_out", ctypes.c_double),
        ("avg_decode_us", ctypes.c_double),
        ("avg_algo_us", ctypes.c_double),
        ("avg_encode_submit_us", ctypes.c_double),
        ("avg_sync_us", ctypes.c_double),
        ("current_queue_packets", ctypes.c_int),
        ("current_queue_surfaces", ctypes.c_int),
        ("last_error_code", ctypes.c_int),
        ("last_error_msg", ctypes.c_char * 256),
        ("input_bytes", ctypes.c_uint64),
        ("output_bytes", ctypes.c_uint64),
        ("decode_errors", ctypes.c_uint64),
        ("encode_errors", ctypes.c_uint64),
        ("route_count", ctypes.c_int),
        ("abi_version", ctypes.c_int),
        ("preprocess_frames", ctypes.c_uint64),
        ("smooth_scale_frames", ctypes.c_uint64),
        ("pre_denoise_frames", ctypes.c_uint64),
        ("scene_analyzed_frames", ctypes.c_uint64),
        ("adaptive_profile_switches", ctypes.c_uint64),
        ("adaptive_qp_frames", ctypes.c_uint64),
        ("mbqp_frames", ctypes.c_uint64),
        ("mbqp_fallback_frames", ctypes.c_uint64),
        ("avg_preprocess_ms", ctypes.c_double),
        ("avg_scene_analyze_ms", ctypes.c_double),
        ("avg_mbqp_build_ms", ctypes.c_double),
        ("mbqp_supported", ctypes.c_int),
        ("mbqp_disabled_reason", ctypes.c_int),
        ("active_profile", ctypes.c_int),
        ("active_algo_flags", ctypes.c_int),
        ("current_input_queue_packets", ctypes.c_int),
        ("async_mode", ctypes.c_int),
        ("async_enqueued_packets", ctypes.c_uint64),
        ("async_processed_packets", ctypes.c_uint64),
        ("backpressure_events", ctypes.c_uint64),
    ]


class MFX50RTAlgoConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("enable_preprocess", ctypes.c_int),
        ("enable_smooth_scale", ctypes.c_int),
        ("smooth_scale_factor", ctypes.c_int),
        ("enable_pre_denoise", ctypes.c_int),
        ("pre_denoise_strength", ctypes.c_int),
        ("enable_scene_analyzer", ctypes.c_int),
        ("enable_adaptive_profile", ctypes.c_int),
        ("enable_adaptive_qp", ctypes.c_int),
        ("enable_mbqp", ctypes.c_int),
        ("mbqp_strength", ctypes.c_int),
        ("mbqp_block_size", ctypes.c_int),
        ("target_output_ratio_permille", ctypes.c_int),
        ("fallback_profile", ctypes.c_int),
        ("aggressive_profile", ctypes.c_int),
        ("reserved", ctypes.c_int * 32),
    ]


class MFX50RTAlgoCaps(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("supports_preprocess", ctypes.c_int),
        ("supports_scene_analyzer", ctypes.c_int),
        ("supports_adaptive_qp", ctypes.c_int),
        ("supports_mbqp", ctypes.c_int),
        ("supports_hevc_mbqp", ctypes.c_int),
        ("supports_runtime_qp_ctrl", ctypes.c_int),
        ("reserved", ctypes.c_int * 32),
    ]


_CALLBACK = ctypes.CFUNCTYPE(
    None,
    ctypes.POINTER(MFX50RTEncodedPacket),
    ctypes.c_void_p,
)


class MFX50RealtimeEncoder:
    def __init__(
        self,
        lib_path: str | Path,
        *,
        route_count: int = 1,
        profile: int = MFX50_PROFILE_QUALITY_90_NEAR,
        input_codec: int = MFX50_CODEC_H264,
        device_selector: bytes = b"auto",
        fps_num: int = 30,
        fps_den: int = 1,
        async_mode: bool = False,
        max_input_queue_packets: int = 256,
        max_output_queue_packets: int = 256,
        algo_config: Optional[MFX50RTAlgoConfig] = None,
        output_callback: Optional[Callable[[int, bytes, int, int], None]] = None,
    ) -> None:
        self._lib = ctypes.CDLL(str(lib_path))
        self._bind()
        self._callback_ref = None
        self._user_callback = output_callback

        cfg = MFX50RTConfig()
        rc = self._lib.MFX50RT_DefaultConfig(ctypes.byref(cfg))
        if rc != MFX50_OK:
            raise RuntimeError("MFX50RT_DefaultConfig failed")
        cfg.route_count = route_count
        cfg.profile = profile
        cfg.input_codec = input_codec
        cfg.output_codec = MFX50_CODEC_HEVC
        cfg.device_selector = device_selector
        cfg.fps_num = fps_num
        cfg.fps_den = fps_den
        cfg.async_mode = 1 if async_mode else 0
        cfg.max_input_queue_packets = max_input_queue_packets
        cfg.max_output_queue_packets = max_output_queue_packets
        cfg.drop_policy = MFX50RT_DROP_NONE
        cfg.output_mode = MFX50_OUTPUT_CALLBACK if output_callback else MFX50_OUTPUT_POLL

        handle = ctypes.c_void_p()
        rc = self._lib.MFX50RT_Create(ctypes.byref(cfg), ctypes.byref(handle))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error(None))
        self._handle = handle

        if algo_config is not None:
            self.set_algo_config(algo_config)

        if output_callback:
            self._callback_ref = _CALLBACK(self._on_output)
            rc = self._lib.MFX50RT_SetOutputCallback(self._handle, self._callback_ref, None)
            if rc != MFX50_OK:
                err = self.last_error()
                self.close()
                raise RuntimeError(err)

    def _bind(self) -> None:
        self._lib.MFX50RT_GetVersion.argtypes = []
        self._lib.MFX50RT_GetVersion.restype = ctypes.c_char_p
        self._lib.MFX50RT_GetAbiVersion.argtypes = []
        self._lib.MFX50RT_GetAbiVersion.restype = ctypes.c_int
        self._lib.MFX50RT_StatusString.argtypes = [ctypes.c_int]
        self._lib.MFX50RT_StatusString.restype = ctypes.c_char_p
        self._lib.MFX50RT_DefaultConfig.argtypes = [ctypes.POINTER(MFX50RTConfig)]
        self._lib.MFX50RT_DefaultConfig.restype = ctypes.c_int
        self._lib.MFX50RT_DefaultAlgoConfig.argtypes = [ctypes.POINTER(MFX50RTAlgoConfig)]
        self._lib.MFX50RT_DefaultAlgoConfig.restype = ctypes.c_int
        self._lib.MFX50RT_Create.argtypes = [ctypes.POINTER(MFX50RTConfig), ctypes.POINTER(ctypes.c_void_p)]
        self._lib.MFX50RT_Create.restype = ctypes.c_int
        self._lib.MFX50RT_SetAlgoConfig.argtypes = [ctypes.c_void_p, ctypes.POINTER(MFX50RTAlgoConfig)]
        self._lib.MFX50RT_SetAlgoConfig.restype = ctypes.c_int
        self._lib.MFX50RT_GetAlgoConfig.argtypes = [ctypes.c_void_p, ctypes.POINTER(MFX50RTAlgoConfig)]
        self._lib.MFX50RT_GetAlgoConfig.restype = ctypes.c_int
        self._lib.MFX50RT_GetAlgoCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(MFX50RTAlgoCaps)]
        self._lib.MFX50RT_GetAlgoCaps.restype = ctypes.c_int
        self._lib.MFX50RT_SetOutputCallback.argtypes = [ctypes.c_void_p, _CALLBACK, ctypes.c_void_p]
        self._lib.MFX50RT_SetOutputCallback.restype = ctypes.c_int
        self._lib.MFX50RT_PushPacket.argtypes = [ctypes.c_void_p, ctypes.POINTER(MFX50RTPacket)]
        self._lib.MFX50RT_PushPacket.restype = ctypes.c_int
        self._lib.MFX50RT_PollPacket.argtypes = [ctypes.c_void_p, ctypes.POINTER(MFX50RTEncodedPacket)]
        self._lib.MFX50RT_PollPacket.restype = ctypes.c_int
        self._lib.MFX50RT_Flush.argtypes = [ctypes.c_void_p]
        self._lib.MFX50RT_Flush.restype = ctypes.c_int
        self._lib.MFX50RT_GetStats.argtypes = [ctypes.c_void_p, ctypes.POINTER(MFX50RTStats)]
        self._lib.MFX50RT_GetStats.restype = ctypes.c_int
        self._lib.MFX50RT_GetLastError.argtypes = [ctypes.c_void_p]
        self._lib.MFX50RT_GetLastError.restype = ctypes.c_char_p
        self._lib.MFX50RT_Close.argtypes = [ctypes.c_void_p]
        self._lib.MFX50RT_Close.restype = ctypes.c_int

    def _on_output(self, pkt_ptr, user) -> None:
        pkt = pkt_ptr.contents
        data = ctypes.string_at(pkt.data, pkt.size)
        if self._user_callback:
            self._user_callback(pkt.stream_id, data, pkt.pts, pkt.frame_type)

    def push_packet(self, stream_id: int, packet: bytes, pts: int = 0, *, end_of_stream: bool = False) -> int:
        arr = (ctypes.c_uint8 * len(packet)).from_buffer_copy(packet) if packet else None
        pkt = MFX50RTPacket()
        pkt.struct_size = ctypes.sizeof(MFX50RTPacket)
        pkt.stream_id = stream_id
        pkt.data = arr
        pkt.size = len(packet)
        pkt.pts = pts
        pkt.dts = pts
        pkt.end_of_stream = 1 if end_of_stream else 0
        rc = self._lib.MFX50RT_PushPacket(self._handle, ctypes.byref(pkt))
        if rc < 0:
            raise RuntimeError(self.last_error())
        return rc

    def poll_packet(self, capacity: int = 8 * 1024 * 1024) -> tuple[int, bytes, int, int] | None:
        buf = (ctypes.c_uint8 * capacity)()
        pkt = MFX50RTEncodedPacket()
        pkt.struct_size = ctypes.sizeof(MFX50RTEncodedPacket)
        pkt.data = buf
        pkt.capacity = capacity
        rc = self._lib.MFX50RT_PollPacket(self._handle, ctypes.byref(pkt))
        if rc == MFX50_ERR_NO_OUTPUT:
            return None
        if rc == MFX50_ERR_BUFFER_TOO_SMALL or rc == MFX50_ERR_AGAIN:
            return self.poll_packet(int(pkt.size))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error())
        return pkt.stream_id, bytes(buf[: pkt.size]), pkt.pts, pkt.frame_type

    def flush(self) -> None:
        rc = self._lib.MFX50RT_Flush(self._handle)
        if rc < 0:
            raise RuntimeError(self.last_error())

    def stats(self) -> MFX50RTStats:
        stats = MFX50RTStats()
        stats.struct_size = ctypes.sizeof(MFX50RTStats)
        rc = self._lib.MFX50RT_GetStats(self._handle, ctypes.byref(stats))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error())
        return stats

    def default_algo_config(self) -> MFX50RTAlgoConfig:
        cfg = MFX50RTAlgoConfig()
        rc = self._lib.MFX50RT_DefaultAlgoConfig(ctypes.byref(cfg))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error())
        return cfg

    def set_algo_config(self, cfg: MFX50RTAlgoConfig) -> None:
        if cfg.struct_size == 0:
            cfg.struct_size = ctypes.sizeof(MFX50RTAlgoConfig)
        rc = self._lib.MFX50RT_SetAlgoConfig(self._handle, ctypes.byref(cfg))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error())

    def get_algo_config(self) -> MFX50RTAlgoConfig:
        cfg = MFX50RTAlgoConfig()
        cfg.struct_size = ctypes.sizeof(MFX50RTAlgoConfig)
        rc = self._lib.MFX50RT_GetAlgoConfig(self._handle, ctypes.byref(cfg))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error())
        return cfg

    def get_algo_caps(self) -> MFX50RTAlgoCaps:
        caps = MFX50RTAlgoCaps()
        caps.struct_size = ctypes.sizeof(MFX50RTAlgoCaps)
        rc = self._lib.MFX50RT_GetAlgoCaps(self._handle, ctypes.byref(caps))
        if rc != MFX50_OK:
            raise RuntimeError(self.last_error())
        return caps

    def version(self) -> str:
        raw = self._lib.MFX50RT_GetVersion()
        return raw.decode("utf-8", errors="replace") if raw else ""

    def abi_version(self) -> int:
        return int(self._lib.MFX50RT_GetAbiVersion())

    def status_string(self, code: int) -> str:
        raw = self._lib.MFX50RT_StatusString(code)
        return raw.decode("utf-8", errors="replace") if raw else ""

    def last_error(self, handle="default") -> str:
        raw = self._lib.MFX50RT_GetLastError(self._handle if handle == "default" else handle)
        return raw.decode("utf-8", errors="replace") if raw else ""

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle:
            self._lib.MFX50RT_Close(handle)
            self._handle = None

    def __enter__(self) -> "MFX50RealtimeEncoder":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()
