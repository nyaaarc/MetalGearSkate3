"""Smoke test for skate_ffi.dll outside the game: flat floor, put the skater on it, push, and
check the root moves.

    python -I tools/ffi_smoke.py <converted skate data dir> [path to skate_ffi.dll]

The data dir is the converter's output (the folder that contains assets/). Positions are in the
engine's own units and axes; this only checks that the session runs and responds to input.
"""
import ctypes
import sys
from pathlib import Path

DEFAULT_DLL = Path(__file__).resolve().parents[1] / "skate-ffi/target/x86_64-pc-windows-msvc/release/skate_ffi.dll"
XINPUT_A = 0x1000


class Controls(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_uint16), ("left_trigger", ctypes.c_uint8), ("right_trigger", ctypes.c_uint8),
                ("left", ctypes.c_int16 * 2), ("right", ctypes.c_int16 * 2)]


def load(dll_path):
    lib = ctypes.CDLL(str(dll_path))
    lib.mgsk_version.restype = ctypes.c_char_p
    lib.mgsk_last_error.restype = ctypes.c_char_p
    lib.mgsk_session_new.restype = ctypes.c_void_p
    lib.mgsk_session_new.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_uint32,
                                     ctypes.POINTER(ctypes.c_float), ctypes.c_float]
    lib.mgsk_install_world.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_uint32]
    lib.mgsk_period.restype = ctypes.c_float
    lib.mgsk_period.argtypes = [ctypes.c_void_p]
    lib.mgsk_activate.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_float]
    lib.mgsk_tick.argtypes = [ctypes.c_void_p, ctypes.POINTER(Controls)]
    lib.mgsk_root.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]
    lib.mgsk_session_free.argtypes = [ctypes.c_void_p]
    return lib


def floor(half=200.0, y=0.0):
    """Two triangles making a flat square at height y, half-size `half` engine units."""
    a, b, c, d = (-half, y, -half), (half, y, -half), (half, y, half), (-half, y, half)
    tris = [a, c, b, a, d, c]
    flat = [x for p in tris for x in p]
    return (ctypes.c_float * len(flat))(*flat), len(tris) // 3


def root_position(lib, session):
    m = (ctypes.c_float * 16)()
    if lib.mgsk_root(session, m):
        raise SystemExit(f"mgsk_root failed: {lib.mgsk_last_error().decode()}")
    return m[12], m[13], m[14]


def main():
    data = sys.argv[1]
    lib = load(sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DLL)
    print(lib.mgsk_version().decode())
    tris, count = floor()
    spawn = (ctypes.c_float * 3)(0, 0, 0)
    session = lib.mgsk_session_new(data.encode(), tris, count, spawn, 0.0)
    if not session:
        raise SystemExit(f"mgsk_session_new failed: {lib.mgsk_last_error().decode()}")
    print(f"session ok, period {lib.mgsk_period(session):.4f} s")
    # Swap in a raised floor, as the plugin does when skating starts, and spawn on it.
    raised, raised_count = floor(y=5.0)
    if lib.mgsk_install_world(session, raised, raised_count):
        raise SystemExit(f"mgsk_install_world failed: {lib.mgsk_last_error().decode()}")
    spawn = (ctypes.c_float * 3)(0, 5.0, 0)
    if lib.mgsk_activate(session, spawn, 0.0):
        raise SystemExit(f"mgsk_activate failed: {lib.mgsk_last_error().decode()}")
    start = root_position(lib, session)
    print("start", tuple(round(v, 2) for v in start))
    neutral, push = Controls(), Controls(buttons=XINPUT_A)
    for tick in range(600):
        # Tap A (push) for 10 ticks every 60.
        if lib.mgsk_tick(session, push if tick % 60 < 10 else neutral):
            raise SystemExit(f"mgsk_tick failed at {tick}: {lib.mgsk_last_error().decode()}")
        if tick % 120 == 119:
            print(f"tick {tick + 1}", tuple(round(v, 2) for v in root_position(lib, session)))
    end = root_position(lib, session)
    moved = sum((e - s) ** 2 for s, e in zip(start, end)) ** 0.5
    print(f"moved {moved:.2f} units")
    lib.mgsk_session_free(session)
    sys.exit(0 if moved > 0.5 else 1)


if __name__ == "__main__":
    main()
