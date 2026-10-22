#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare disposable ordinary files and their independent manifest on the Mac.

This never opens a device. Existing records are verified and reused unchanged.
The record must remain under the repository's build/capture-roundtrip folder.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from image_write_integration import require
from test_physical_capture_usb import WORK, verify_fixture


def prepare(record):
    WORK.mkdir(parents=True, exist_ok=True)
    require(record.resolve().parent == WORK.resolve() and not record.is_symlink(),
            "fixture record must be a local file under build/capture-roundtrip")
    if record.exists():
        fixture = json.loads(record.read_text())
        verify_fixture(Path(fixture["payload"]), json.loads(Path(fixture["manifest"]).read_text()))
        return fixture
    folder = Path(tempfile.mkdtemp(prefix="dummy-files-", dir=WORK)).resolve()
    payload = folder / "payload"
    payload.mkdir()

    def put(name, data):
        path = payload / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def pattern(name, size):
        block = hashlib.sha256(name.encode("utf-8")).digest()
        put(name, (block * ((size + len(block) - 1) // len(block)))[:size])

    put("hello.txt", b"Rufus++ capture and restore fixture. This is disposable test data.\n")
    put("nested/one/two/notes.txt", b"Nested folders must survive the capture and restore.\n")
    put("unicode/caf\u00e9-\u65e5\u672c\u8a9e-\U0001f600.txt",
        "Unicode filenames and UTF-8 contents: caf\u00e9 \u65e5\u672c\u8a9e \U0001f600\n".encode("utf-8"))
    put("long-names/" + "long filename with spaces " * 4 + ".txt", b"Long filename test\n")
    put("empty.bin", b"")
    (payload / "empty-directory").mkdir()
    put("binary/every-byte.bin", bytes(range(256)) * 1024)
    put("binary/zeroes.bin", bytes(1024**2))
    pattern("binary/large-pattern.bin", 2 * 1024**2 + 513)
    for size in (1, 511, 512, 513, 4095, 4096, 4097, 65535, 65536, 65537):
        pattern("boundaries/size-{}.bin".format(size), size)
    for index in range(64):
        pattern("many-files/file-{:03d}.txt".format(index), 1024 + index)
    files = [{"path": path.relative_to(payload).as_posix(), "size": path.stat().st_size,
              "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
             for path in sorted(payload.rglob("*")) if path.is_file()]
    manifest = {"purpose": "Non-bootable physical USB capture/restore fixture",
                "files": files, "directories": [path.relative_to(payload).as_posix()
                    for path in sorted(payload.rglob("*")) if path.is_dir()],
                "fileCount": len(files), "totalFileBytes": sum(entry["size"] for entry in files)}
    manifest_path = folder / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    verify_fixture(payload, manifest)
    fixture = {"fixture": str(folder), "payload": str(payload), "manifest": str(manifest_path),
               "fileCount": manifest["fileCount"], "totalFileBytes": manifest["totalFileBytes"],
               "localFixtureHashesVerified": True, "physicalUsbModified": False}
    # Exclusive creation preserves an existing record, including one prepared
    # for a currently running physical test.
    with record.open("x", encoding="utf-8") as output:
        output.write(json.dumps(fixture, indent=2) + "\n")
    return fixture


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", type=Path, default=WORK / "fixture-ready.json")
    args = parser.parse_args()
    print(json.dumps(prepare(args.record), indent=2))
