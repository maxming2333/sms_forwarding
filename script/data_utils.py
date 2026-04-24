"""
Shared helper: minify HTML + gzip-compress all files in a data/ directory.

Used by both compress_data.py (CI/build) and upload_littlefs.py (PlatformIO
post-upload SCons script) to keep compression logic in one place.
"""

import gzip
import os
import subprocess
import sys

try:
    import minify_html
    _has_minify_html = True
except ImportError:
    print("[data_utils] 📦 'minify_html' not found, installing automatically...")
    try:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "minify_html"])
        import minify_html
        _has_minify_html = True
        print("[data_utils] ✅ 'minify_html' installed successfully.")
    except Exception as exc:
        print("[data_utils] ⚠️  failed to install 'minify_html': %s" % exc)
        print("[data_utils]     HTML will not be minified. Run: pip install minify_html")
        _has_minify_html = False


def compress_data_dir(data_dir, tag="[data_utils]"):
    """Minify HTML + gzip every non-.gz file in *data_dir*.

    Originals are removed from disk.  Returns a ``{fname: bytes}`` dict of the
    original (pre-minify) file contents so the caller can restore them later.
    """
    originals = {}
    print("%s 🚀 compressing data dir: %s" % (tag, data_dir))
    for fname in os.listdir(data_dir):
        fpath = os.path.join(data_dir, fname)
        if not os.path.isfile(fpath) or fname.endswith(".gz"):
            continue
        with open(fpath, "rb") as f:
            content = f.read()
        originals[fname] = content

        # Minify HTML (including inline JS/CSS) before compression.
        data_to_compress = content
        if fname.endswith(".html") and _has_minify_html:
            try:
                minified = minify_html.minify(
                    content.decode("utf-8"),
                    minify_js=True,
                    minify_css=True,
                    remove_processing_instructions=True,
                ).encode("utf-8")
                print(
                    "%s 🗜️  minified  %s  (%d -> %d bytes)"
                    % (tag, fname, len(content), len(minified))
                )
                data_to_compress = minified
            except Exception as exc:
                print("%s ⚠️  minify failed for %s: %s" % (tag, fname, exc))

        gz_path = fpath + ".gz"
        with gzip.open(gz_path, "wb") as gf:
            gf.write(data_to_compress)
        os.remove(fpath)
        print(
            "%s 📦 gzipped  %s -> %s.gz  (%d -> %d bytes)"
            % (tag, fname, fname, len(data_to_compress), os.path.getsize(gz_path))
        )

    return originals
