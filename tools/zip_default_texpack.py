import os
import zipfile

root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
src = os.path.join(root, "src", "preload", "texpacks", "default")
out = os.path.join(root, "src", "preload", "texpacks", "default.zip")

if not os.path.isdir(src):
    raise SystemExit(f"Missing folder: {src}")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for folder, _, files in os.walk(src):
        for file in files:
            path = os.path.join(folder, file)
            rel = os.path.relpath(path, src)
            z.write(path, rel)

print(f"Created {out}")