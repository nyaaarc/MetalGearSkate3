"""Exports the skater's sounds from the player's own Skate 3 disc for skate_ffi.dll's audio host:
decodes the sound banks, rolling grains and wheel recordings to WAV and copies the AEMS banks,
projects, MixMap and Splice trees, writing <assets>/private/audio/audio_manifest.json.

    python -I tools/skate_audio_export/run.py <extracted disc> <assets dir> <vgmstream-cli.exe> [--work DIR]

<extracted disc> holds default.xex and data/; <assets dir> is the converted data's folder (the one
skate_data points at). Needs numpy. The pipeline is SK8-ENGINE/skate-3-rust-engine's
`tools/asset_pipeline/audio_export.py` (see README.md here); nothing is written to the disc copy.
"""
import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tools.asset_pipeline import audio_export  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disc", type=Path)
    ap.add_argument("assets", type=Path)
    ap.add_argument("vgmstream", type=Path)
    ap.add_argument("--work", type=Path, help="scratch folder (default: a temporary one, deleted after)")
    a = ap.parse_args()
    if not (a.disc / "data" / "audio").is_dir():
        raise SystemExit(f"{a.disc} has no data/audio (pass the extracted Xbox 360 disc folder)")
    if not a.vgmstream.is_file():
        raise SystemExit(f"{a.vgmstream} not found")
    work = a.work or Path(tempfile.mkdtemp(prefix="mgsk-audio-"))
    started = time.time()
    try:
        manifest = audio_export.convert(
            a.disc, a.assets / "private", work, a.vgmstream,
            report=lambda m: print(f"  [{time.time() - started:5.0f} s] {m}", flush=True),
            log=sys.stdout.buffer)
    finally:
        if a.work is None:
            shutil.rmtree(work, ignore_errors=True)
    print(f"audio: {len(manifest['banks'])} sample banks, {len(manifest['grains'])} rolling grains -> "
          f"{a.assets / 'private' / 'audio'}")


if __name__ == "__main__":
    main()
