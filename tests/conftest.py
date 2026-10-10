import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEV_BUILD = ROOT / "build/linux-dev"


@pytest.fixture(scope="session")
def dev_build() -> pathlib.Path:
    if shutil.which("cmake") is None or shutil.which("ninja") is None:
        pytest.skip("needs cmake and ninja")
    subprocess.run(["cmake", "--preset", "linux-dev"], cwd=ROOT, check=True, capture_output=True)
    result = subprocess.run(
        ["cmake", "--build", "--preset", "linux-dev"], cwd=ROOT, capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, result.stdout[-3000:]
    return DEV_BUILD


@pytest.fixture(scope="session")
def rusty_headless(dev_build: pathlib.Path) -> pathlib.Path:
    return dev_build / "rusty_headless"
