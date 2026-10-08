"""Minimal stand-in for upstream's install module: only what audio_export uses."""
import subprocess
from pathlib import Path

from tools.owned_game.big import BigArchive

TOOLS = Path(__file__).resolve().parents[1]


def spawn(args, **popen):
    return subprocess.Popen([str(a) for a in args], **popen)


def extract(archive, destination, entries=None):
    data = BigArchive(archive)
    data.extract_entries(data.entries if entries is None else [e for e in data.entries if entries(e)], destination)
    return data
