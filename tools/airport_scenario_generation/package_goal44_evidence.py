"""Build a deterministic portable archive and checksum ledger for Goal 44."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "docs" / "goal44-evidence"
ARCHIVE = EVIDENCE / "bundle.zip"
LEDGER = EVIDENCE / "SHA256SUMS.txt"
ARCHIVE_HASH = EVIDENCE / "bundle-archive-sha256.txt"
TEMP_ARCHIVE = EVIDENCE / "bundle.tmp.zip"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def payload_files() -> list[Path]:
    excluded = {ARCHIVE, LEDGER, ARCHIVE_HASH, TEMP_ARCHIVE}
    return sorted((path for path in EVIDENCE.rglob("*") if path.is_file() and path not in excluded),
                  key=lambda path: path.relative_to(EVIDENCE).as_posix())


def write_archive() -> str:
    payload = payload_files()
    LEDGER.write_text("".join(
        f"{sha256(path)}  {path.relative_to(EVIDENCE).as_posix()}\n" for path in payload),
        encoding="utf-8")

    files = sorted([*payload, LEDGER], key=lambda path: path.relative_to(EVIDENCE).as_posix())
    with zipfile.ZipFile(TEMP_ARCHIVE, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            relative = path.relative_to(EVIDENCE).as_posix()
            info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    os.replace(TEMP_ARCHIVE, ARCHIVE)
    archive_digest = sha256(ARCHIVE)
    ARCHIVE_HASH.write_text(f"{archive_digest}  bundle.zip\n", encoding="utf-8")

    expected = {path.relative_to(EVIDENCE).as_posix() for path in files}
    with zipfile.ZipFile(ARCHIVE) as archive:
        bad_member = archive.testzip()
        if bad_member is not None:
            raise ValueError(f"ZIP CRC failed for {bad_member}")
        if set(archive.namelist()) != expected:
            raise ValueError("ZIP members differ from the checksum ledger payload")
        for line in LEDGER.read_text(encoding="utf-8").splitlines():
            expected_digest, relative = line.split("  ", 1)
            if sha256(EVIDENCE / relative) != expected_digest:
                raise ValueError(f"checksum mismatch for {relative}")
            if hashlib.sha256(archive.read(relative)).hexdigest() != expected_digest:
                raise ValueError(f"archived checksum mismatch for {relative}")
    if sha256(ARCHIVE) != ARCHIVE_HASH.read_text(encoding="utf-8").split()[0]:
        raise ValueError("archive SHA-256 sidecar mismatch")
    return archive_digest


if __name__ == "__main__":
    print(f"archive={ARCHIVE.relative_to(ROOT).as_posix()}")
    print(f"archive_sha256={write_archive()}")
    print(f"payload_files={len(payload_files())}")
