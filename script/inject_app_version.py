Import("env")
import os
import subprocess
from datetime import datetime, timezone

project_dir = env["PROJECT_DIR"]

# --- APP_VERSION ---
app_version = os.environ.get("APP_VERSION", "").strip()
if not app_version:
    version_file = os.path.join(project_dir, "VERSION")
    if os.path.exists(version_file):
        prefix = open(version_file).read().strip()
    else:
        prefix = "v1"
    if not prefix:
        prefix = "unknown"
        print("[inject_app_version] [WARN] VERSION file is empty, using 'unknown'")
    if not prefix.lower().startswith("v"):
        prefix = "v" + prefix
    try:
        sha = subprocess.check_output(
            ["git", "rev-parse", "--short=7", "HEAD"],
            cwd=project_dir, stderr=subprocess.DEVNULL
        ).decode().strip()
        app_version = f"{prefix}-{sha}"
    except Exception:
        app_version = f"{prefix}-local"
        print("[inject_app_version] [WARN] git not available, using '-local' suffix")

# --- APP_BUILD_DATE / APP_BUILD_TIME ---
ts = os.environ.get("APP_BUILD_TIMESTAMP", "").strip()
if ts and len(ts) >= 15 and ts[8] == 'T':
    app_build_date = f"{ts[0:4]}-{ts[4:6]}-{ts[6:8]}"
    app_build_time = f"{ts[9:11]}:{ts[11:13]}:{ts[13:15]}"
else:
    if ts:
        print(f"[inject_app_version] [WARN] APP_BUILD_TIMESTAMP format invalid: '{ts}', using UTC now")
    now = datetime.now(timezone.utc)
    app_build_date = now.strftime("%Y-%m-%d")
    app_build_time = now.strftime("%H:%M:%S")

# --- Inject ---
env.Append(CPPDEFINES=[
    ("APP_VERSION",    '\\"' + app_version    + '\\"'),
    ("APP_BUILD_DATE", '\\"' + app_build_date + '\\"'),
    ("APP_BUILD_TIME", '\\"' + app_build_time + '\\"'),
])

print(f"[inject_app_version] APP_VERSION={app_version}")
print(f"[inject_app_version] APP_BUILD_DATE={app_build_date}")
print(f"[inject_app_version] APP_BUILD_TIME={app_build_time}")
