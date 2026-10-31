"""Run the same two official offline exporters as system-update-bundle.

The full low-noise firmware build must have succeeded first. No device is used.
"""
from pathlib import Path
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parent
UTILS = ROOT / "submodule/esp-mosaico-utils"
PROJECT = ROOT / "projects/wordquest"
BUILD = PROJECT / "build"
PYTHON = ROOT / ".venv-iris/Scripts/python.exe"
STAGE = BUILD / "system-update"
BUNDLE = BUILD / "wordquest-system-update.irisfw"
env = dict(os.environ)
for key in ("MSYSTEM", "MINGW_PREFIX", "PYTHONPATH", "PYTHONHOME"):
    env.pop(key, None)
env.update(PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
commands = [
    [str(PYTHON), str(UTILS / "mosaico-tools/tools/prepare_system_update.py"), "--partition-csv", str(PROJECT / "partitions.csv"), "--partition-table", str(BUILD / "partition_table/partition-table.bin"), "--application", str(BUILD / "wordquest.bin"), "--ui-apps", str(BUILD / "ui_apps.bin"), "--stage-dir", str(STAGE), "--release", "1.0.0"],
    [str(PYTHON), str(UTILS / "ESP-Iris/components/esp_iris/tools/esp_iris.py"), "bundle", "build", str(STAGE / "manifest.json"), "--component-root", str(STAGE), "--output", str(BUNDLE)],
]
(ROOT / "evidence/package-commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
with (ROOT / "evidence/package.log").open("w", encoding="utf-8") as output:
    for command in commands:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            raise SystemExit(result.returncode)
print(str(BUNDLE))
