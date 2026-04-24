import os
import json

data_dir = 'data'
print('[restore_data] 🔄 restoring original files from .pio/originals.json...')
with open('.pio/originals.json', 'r') as jf:
    originals = json.load(jf)
for fname in os.listdir(data_dir):
    if fname.endswith('.gz'):
        os.remove(os.path.join(data_dir, fname))
        print('[restore_data] 🗑️  removed %s' % fname)
for fname, content_list in originals.items():
    fpath = os.path.join(data_dir, fname)
    with open(fpath, 'wb') as f:
        f.write(bytes(content_list))
    print('[restore_data] ✅ restored %s (%d bytes)' % (fname, len(content_list)))
print('[restore_data] ✅ all files restored')
