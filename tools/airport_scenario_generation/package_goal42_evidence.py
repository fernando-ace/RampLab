"""Create and verify the portable Goal 42 evidence archive and SHA ledger."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "docs" / "goal42-evidence"
ARCHIVE = EVIDENCE / "goal42-portable-evidence.zip"
LEDGER = EVIDENCE / "SHA256SUMS.txt"
SIDECAR = EVIDENCE / "goal42-portable-evidence.zip.sha256"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def package() -> dict[str, object]:
    for required in (EVIDENCE / "README.md", EVIDENCE / "validation.json", EVIDENCE / "run" / "control" / "events.jsonl",
                     EVIDENCE / "run" / "disruption" / "events.jsonl", EVIDENCE / "geometry" / "kauo_goal42.geometry.json",
                     EVIDENCE / "correlation.json"):
        if not required.is_file():
            raise FileNotFoundError(f"required Goal 42 evidence is missing: {required.relative_to(ROOT)}")
    validation = json.loads((EVIDENCE / "validation.json").read_text(encoding="utf-8"))
    validation["portable_evidence"] = {"status": "PASS", "archive": ARCHIVE.name,
                                       "checksum_ledger": LEDGER.name, "archive_digest_sidecar": SIDECAR.name}
    (EVIDENCE / "validation.json").write_text(json.dumps(validation, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    excluded = {ARCHIVE, LEDGER, SIDECAR}
    files = sorted((path for path in EVIDENCE.rglob("*") if path.is_file() and path not in excluded),
                   key=lambda path: path.relative_to(EVIDENCE).as_posix())
    ledger_lines = [f"{_sha256(path)}  {path.relative_to(EVIDENCE).as_posix()}" for path in files]
    LEDGER.write_text("\n".join(ledger_lines) + "\n", encoding="utf-8")
    with ZipFile(ARCHIVE, "w", compression=ZIP_DEFLATED) as archive:
        for path in files:
            archive.write(path, path.relative_to(EVIDENCE).as_posix())
        archive.write(LEDGER, LEDGER.relative_to(EVIDENCE).as_posix())

    checked = 0
    for line in LEDGER.read_text(encoding="utf-8").splitlines():
        expected, relative = line.split("  ", 1)
        path = (EVIDENCE / relative).resolve()
        if EVIDENCE.resolve() not in path.parents:
            raise ValueError(f"checksum path escapes evidence root: {relative}")
        if _sha256(path) != expected:
            raise ValueError(f"checksum mismatch: {relative}")
        checked += 1
    with ZipFile(ARCHIVE) as archive:
        bad_member = archive.testzip()
        if bad_member:
            raise ValueError(f"ZIP CRC failed for {bad_member}")
        expected_members = sorted([path.relative_to(EVIDENCE).as_posix() for path in files] + [LEDGER.name])
        if sorted(archive.namelist()) != expected_members:
            raise ValueError("portable archive members differ from the hashed evidence set")
    archive_digest = _sha256(ARCHIVE)
    SIDECAR.write_text(f"{archive_digest}  {ARCHIVE.name}\n", encoding="utf-8")
    return {"status": "PASS", "files_verified": checked, "zip_integrity": "PASS",
            "archive_sha256": archive_digest, "archive_path": str(ARCHIVE)}


if __name__ == "__main__":
    print(json.dumps(package(), indent=2, sort_keys=True))
