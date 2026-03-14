#!/usr/bin/env python3
import json
from pathlib import Path


PROJECT = Path(__file__).resolve().parent.parent
MANIFEST = PROJECT / "tools" / "test_manifest.json"
OUTPUT = PROJECT / "docs" / "review" / "2026-03-13-test-coverage-matrix.md"


def load_json(path):
    with path.open() as fh:
        return json.load(fh)


def main():
    manifest = load_json(MANIFEST)
    categories = manifest["categories"]

    lines = [
        "# Test Coverage Matrix",
        "",
        "Generated from `tools/test_manifest.json`.",
        "",
        "| ID | Category | Local Result | Layer | Status | Tests | Commands | Notes |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]

    covered = 0
    blocked = 0
    manual = 0
    failed = 0
    not_run = 0

    def local_result(entry):
        if "local_result" in entry:
            return entry["local_result"]
        status = entry["status"]
        if status == "blocked_external":
            return "blocked_external"
        if status == "manual_pty":
            return "manual_pty"
        if status == "covered":
            return "passed"
        return "not_run"

    for entry in categories:
        status = entry["status"]
        result = local_result(entry)
        if status == "blocked_external":
            blocked += 1
        elif status == "manual_pty":
            manual += 1
        else:
            covered += 1
        if result == "failed":
            failed += 1
        elif result == "not_run":
            not_run += 1
        lines.append(
            "| {id} | {name} | {result} | {layer} | {status} | {tests} | {commands} | {notes} |".format(
                id=entry["id"],
                name=entry["name"].replace("|", "/"),
                result=result,
                layer=entry["layer"],
                status=status,
                tests=", ".join(entry["tests"]).replace("|", "/"),
                commands="<br>".join(entry["commands"]).replace("|", "/"),
                notes=entry["notes"].replace("|", "/"),
            )
        )

    lines.extend([
        "",
        f"- Locally passed or mapped through automated runs: {covered}",
        f"- Blocked on external third-party payloads: {blocked}",
        f"- Reserved for manual PTY coverage: {manual}",
        f"- Categories with a current local failing result: {failed}",
        f"- Categories mapped but not yet run locally: {not_run}",
        "",
        "Blocked categories are still addressed in the suite through explicit stage commands and prerequisite reporting; they cannot execute until the required external binaries are supplied in `thirdparty/`.",
        "Where interactive PTY automation exists, `local_result` reflects the current smoke-run outcome.",
        "",
    ])

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines))
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
