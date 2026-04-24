"""
Shared helper: gzip-compress all files in a data/ directory.

Used by both compress_data.py (CI/build) and upload_littlefs.py (PlatformIO
post-upload SCons script) to keep compression logic in one place.
"""

import gzip
import os


def compress_data_dir(data_dir, tag="[data_utils]"):
    """Gzip every non-.gz file in *data_dir*.

    Originals are removed from disk. Returns a ``{fname: bytes}`` dict of the
    original file contents so the caller can restore them later.
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

        gz_path = fpath + ".gz"
        with gzip.open(gz_path, "wb") as gf:
            gf.write(content)
        os.remove(fpath)
        print(
            "%s 📦 gzipped  %s -> %s.gz  (%d -> %d bytes)"
            % (tag, fname, fname, len(content), os.path.getsize(gz_path))
        )

    return originals
