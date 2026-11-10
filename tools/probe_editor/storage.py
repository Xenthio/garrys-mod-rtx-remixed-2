"""Shared authoring persistence for desktop and diagnostic browser frontends."""

import json
import os
from pathlib import Path

from . import document


def atomic_write(path, payload):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def save(value, map_hash, output, runtime=None):
    if runtime and Path(runtime).resolve() == Path(output).resolve():
        raise ValueError("Runtime sidecar must differ from the authoring JSON")
    clean = document.validate(value, map_hash)
    # Refuse invalid runtime edits before touching either previously saved file.
    binary = document.runtime_binary(clean) if runtime else None
    atomic_write(
        output, (json.dumps(clean, indent=2, allow_nan=False) + "\n").encode("utf-8")
    )
    if runtime:
        atomic_write(runtime, binary)
    return clean
