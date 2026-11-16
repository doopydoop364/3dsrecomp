"""Compare two screenshots: share of pixels differing by more than a threshold (per channel).
Usage: imgdiff.py a.png b.png [threshold=8] [diff.png]"""
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)
t = int(sys.argv[3]) if len(sys.argv) > 3 else 8
d = np.abs(a - b).max(axis=2)
bad = d > t
print(f"{bad.mean()*100:.3f}% pixels differ by more than {t} (max diff {d.max()})")
if len(sys.argv) > 4:
    out = np.concatenate([a, b, np.stack([bad * 255] * 3, axis=2)], axis=1).astype(np.uint8)
    Image.fromarray(out).save(sys.argv[4])
