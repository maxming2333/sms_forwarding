"""
Post-upload script: gzip-compress files in data/, upload LittleFS filesystem
image, then restore the original uncompressed files.

ESPAsyncWebServer's serveStatic() probes <file>.gz before <file>, so uploading
only the .gz versions saves flash space while keeping development files readable.

Triggered by: pio run -t upload  (or clicking "Upload" in IDE)
"""

Import("env")  # noqa: F821 — SCons injects this

import os
import subprocess
import sys

# Add script/ directory to sys.path so data_utils can be imported from SCons context.
_script_dir = os.path.dirname(os.path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

from data_utils import compress_data_dir  # noqa: E402


def _upload_littlefs(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    data_dir = os.path.join(project_dir, "data")

    if not os.path.isdir(data_dir):
        print("[upload_littlefs] ⚠️  data/ directory not found, skipping filesystem upload")
        return

    # Step 1: Minify + gzip all files in data/ (originals kept in memory).
    originals = compress_data_dir(data_dir, tag="[upload_littlefs]")

    # Step 2: Upload LittleFS (only .gz files are present now).
    print("\n[upload_littlefs] 🚀 uploading filesystem image...")
    ret = 1
    try:
        ret = subprocess.call(
            [
                sys.executable, "-m", "platformio",
                "run",
                "--target", "uploadfs",
                "--environment", env["PIOENV"],
            ],
            cwd=project_dir,
        )
    finally:
        # Step 3: Remove generated .gz files and restore originals.
        for fname, content in originals.items():
            fpath = os.path.join(data_dir, fname)
            gz_path = fpath + ".gz"
            if os.path.exists(gz_path):
                os.remove(gz_path)
            with open(fpath, "wb") as f:
                f.write(content)
        print("[upload_littlefs] 🔄 original files restored")

    if ret == 0:
        print("[upload_littlefs] ✅ filesystem upload OK")
    else:
        print("[upload_littlefs] ⚠️  filesystem upload failed (code %d)" % ret)


env.AddPostAction("upload", _upload_littlefs)  # noqa: F821
