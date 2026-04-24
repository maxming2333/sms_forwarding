import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from data_utils import compress_data_dir  # noqa: E402

data_dir = 'data'
os.makedirs('.pio', exist_ok=True)
print('[compress_data] 🚀 compressing data/ for LittleFS...')
originals = compress_data_dir(data_dir, tag='[compress_data]')

# Save originals as byte lists so restore_data.py can reconstruct them.
with open('.pio/originals.json', 'w') as jf:
    json.dump({fname: list(content) for fname, content in originals.items()}, jf)
print('[compress_data] ✅ done, originals saved to .pio/originals.json')
