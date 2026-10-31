"""Publish already-validated local artifacts to the requested release paths."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import zipfile
ROOT = Path(__file__).resolve().parent
PROJECT = ROOT / "projects/wordquest"
ZROOT = Path("Z:/BaiduNetdiskWorkspace/myagent-work/zcode/esp-mosaico-zax")
DEST = ZROOT / "projects/wordquest/docs/release-v02"
BUNDLE = PROJECT / "build/wordquest-system-update.irisfw"
TARGET = ZROOT / "firmware/wordquest-v0.2-system-update.irisfw"
validation = json.loads((ROOT / "evidence/validation.json").read_text())
assert validation["status"] == "PASS"
assert hashlib.sha256(BUNDLE.read_bytes()).hexdigest() == validation["sha256"]
assert not TARGET.exists(), "Refuse to overwrite a release artifact"
DEST.mkdir(parents=True, exist_ok=True)
shutil.copy2(BUNDLE, TARGET)
shutil.copytree(ROOT / "evidence", DEST / "evidence", dirs_exist_ok=True)
shutil.copytree(PROJECT / ".codex-runs/idf-low-noise-build", DEST / "idf-low-noise-build", dirs_exist_ok=True)
for name in ("release_build.py", "release_package.py", "release_verify.py", "release_deliver.py"):
    shutil.copy2(ROOT / name, DEST / name)
for name in ("CMakeLists.txt", "partitions.csv", "sdkconfig.defaults", "sdkconfig.application.defaults", "dependencies.lock"):
    path = DEST / "migrated-inputs" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(PROJECT / name, path)
shutil.copy2(PROJECT / "build/sdkconfig", DEST / "effective-sdkconfig")
shutil.copy2(PROJECT / "build/project_description.json", DEST / "project_description.json")
with zipfile.ZipFile(DEST / "wordquest-v02-build-source.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for prefix in ("main", "ui", "pc"):
        for path in sorted((PROJECT / prefix).rglob("*")):
            if path.is_file():
                archive.write(path, str(path.relative_to(PROJECT)))
    for name in ("CMakeLists.txt", "partitions.csv", "sdkconfig.defaults", "sdkconfig.application.defaults", "dependencies.lock"):
        archive.write(PROJECT / name, name)
reproduction = dict(build_source=str(ROOT), authoritative_source=str(ZROOT / "projects/wordquest"), utils_commit="be151ae047014e79a9c7ae0092dd203844499003", bsp_commit="a2c985ad698b3f04a21c63e006b43e7e948bc1d7", idf_path="C:/Users/zeroz/esp/esp-idf", idf_commit="7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe", idf_tools="C:/es/.espressif", idf_python="C:/es/.espressif/python_env/idf6.2_py3.12_env/Scripts/python.exe", iris_python=str(ROOT / ".venv-iris/Scripts/python.exe"), gsp="1.5.1", gspc="C:/Users/zeroz/esp/tools/gspc-0.7.0/gspc.exe", commands=["python release_build.py doctor", "python release_build.py build", "python release_package.py", str(ROOT / ".venv-iris/Scripts/python.exe") + " release_verify.py"], reproduction_notes=["Restore the source ZIP into projects/wordquest of a separate workspace; never run it in an archive download/extraction directory", "Clone official utils with --no-hardlinks and checkout the pinned commit; clone BSP at the recorded commit and apply evidence/bsp-existing-key-fix.patch", "Install Iris host requirements.lock into an isolated .venv-iris", "Reuse a COPY of managed_components or resolve dependencies.lock through IDF; never share mutable build output", "Scripts sanitize MSYSTEM/MINGW_PREFIX and set tool paths only in Python child environments", "The original Z application and original C build workspace were not migrated in place", "No device operation, upload, commit or push was performed"], bundle=str(TARGET), sha256=validation["sha256"], size=validation["size"])
(DEST / "reproduction.json").write_text(json.dumps(reproduction, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
print(json.dumps(dict(bundle=str(TARGET), evidence=str(DEST), sha256=validation["sha256"], size=validation["size"]), indent=2))
