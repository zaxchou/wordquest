"""Reproduce the isolated Wordquest 0.2 build; never connects to a device."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
UTILS = ROOT / "submodule/esp-mosaico-utils"
PROJECT = ROOT / "projects/wordquest"
IDF = Path("C:/Users/zeroz/esp/esp-idf")
PYTHON = Path("C:/es/.espressif/python_env/idf6.2_py3.12_env/Scripts/python.exe")
RUNNER = UTILS / "mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py"
env = dict(os.environ)
for name in ("MSYSTEM", "MINGW_PREFIX", "IDF_PATH", "IDF_PYTHON_ENV_PATH", "PYTHONPATH", "PYTHONHOME"):
    env.pop(name, None)
env.update(IDF_PATH=str(IDF), IDF_TOOLS_PATH="C:/es/.espressif", IDF_PYTHON_ENV_PATH=str(PYTHON.parent.parent), MOSAICO_IDF_PYTHON=str(PYTHON), GSPC_EXECUTABLE="C:/Users/zeroz/esp/tools/gspc-0.7.0/gspc.exe", ESP_IRIS_PYTHON=str(PYTHON), PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
assert subprocess.check_output(["git", "-C", str(IDF), "rev-parse", "HEAD"], text=True).strip() == "7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe"
assert subprocess.check_output(["git", "-C", str(UTILS), "rev-parse", "HEAD"], text=True).strip() == "be151ae047014e79a9c7ae0092dd203844499003"
args = sys.argv[1:] or ["doctor"]
command = [str(PYTHON), str(RUNNER), "--project", str(PROJECT), "--idf-path", str(IDF), *args]
result = subprocess.run(command, cwd=ROOT, env=env)
raise SystemExit(result.returncode)
