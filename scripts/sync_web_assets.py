Import("env")

import os
import shutil
import gzip
from pathlib import Path

project_dir = Path(env.subst("$PROJECT_DIR"))
configured_web_root = os.environ.get("SUNSYNC_WEB_ROOT")
web_root = Path(configured_web_root) if configured_web_root else project_dir.parent / "SunSync"

if not web_root.is_dir():
    if all((project_dir / "data" / page).is_file()
           for page in ("index.html", "login.html", "dashboard.html")):
        print("Sun Sync web source not found; using the bundled LittleFS files.")
    else:
        raise RuntimeError(
            "Sun Sync web pages are missing. Set SUNSYNC_WEB_ROOT or restore data/."
        )
else:
    for page in ("index.html", "login.html", "dashboard.html"):
        source = web_root / page
        if not source.is_file():
            raise RuntimeError("Required Sun Sync page is missing: " + str(source))
        shutil.copy2(source, project_dir / "data" / page)

    source_resources = web_root / "resources"
    if not source_resources.is_dir():
        raise RuntimeError("Sun Sync resources directory is missing: " + str(source_resources))
    # index.js is device-specific: it calls the ESP32 backend proxy, not the backend directly.
    shutil.copytree(source_resources, project_dir / "data" / "resources", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("index.js"))
    print("Synchronized the three Sun Sync pages and their static resources to LittleFS.")

for source in project_dir.joinpath("data").rglob("*"):
    if source.is_file() and source.suffix.lower() in {".html", ".css", ".js"}:
        compressed = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
        source.with_name(source.name + ".gz").write_bytes(compressed)

print("Generated gzip-compressed HTML, CSS, and JavaScript files for LittleFS.")
