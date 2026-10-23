Import("env")

import gzip
from pathlib import Path

project_dir = Path(env.subst("$PROJECT_DIR"))

# The admin pages are hosted by the backend; data/ only holds the captive portal.
for source in project_dir.joinpath("data").rglob("*"):
    if source.is_file() and source.suffix.lower() in {".html", ".css", ".js"}:
        compressed = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
        source.with_name(source.name + ".gz").write_bytes(compressed)

print("Generated gzip-compressed HTML, CSS, and JavaScript files for LittleFS.")
