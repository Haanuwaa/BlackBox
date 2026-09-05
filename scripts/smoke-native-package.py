#!/usr/bin/env python3
"""Launch an extracted engineering package with isolated state and a deadline."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def verify_report(path, platform, revision):
    fields = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or key in fields:
            raise ValueError(f"Malformed or duplicate diagnostic field: {key}")
        fields[key] = value
    for key, expected in {
        "platform": platform, "source_revision": revision,
        "completed": "1", "failed_samples": "0",
    }.items():
        if fields.get(key) != expected:
            raise ValueError(f"Unexpected {key}: {fields.get(key)!r}, expected {expected!r}")
    if int(fields.get("collections", "0")) < 2:
        raise ValueError("Packaged application did not collect at least two samples")
    return fields


def run_smoke(command, evidence, platform, revision, duration=5, timeout=30):
    evidence = Path(evidence).resolve()
    # A previous report must never turn a failed/new launch into a passing smoke.
    evidence.mkdir(parents=True, exist_ok=False)
    state = evidence / "state"
    state.mkdir()
    report = evidence / "runtime.ini"
    environment = os.environ.copy()
    environment.update({
        "BLACKBOX_PRODUCT_SETTINGS_PATH": str(state / "product-settings.ini"),
        "BLACKBOX_SETTINGS_PATH": str(state / "settings.ini"),
        "XDG_CONFIG_HOME": str(state),
    })
    result = {"completed": False, "platform": platform, "source_revision": revision}
    try:
        with (evidence / "application.log").open("wb") as log:
            subprocess.run(
                [*command, f"--background-diagnostic-seconds={duration}",
                 f"--diagnostic-report={report}"],
                cwd=state, env=environment, stdout=log, stderr=subprocess.STDOUT,
                timeout=timeout, check=True,
            )
        fields = verify_report(report, platform, revision)
        result.update(completed=True, collections=int(fields["collections"]))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        raise
    finally:
        (evidence / "result.json").write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--platform", required=True, choices=["Linux", "macOS"])
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    try:
        run_smoke([str(args.executable.resolve())], args.evidence, args.platform, args.revision)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Native package smoke failed: {error}", file=sys.stderr)
        return 1
    print(f"{args.platform} extracted package smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
