"""Contact sheet of build/shot_*.png (full frames, half size) -> build/sheet.png. Run with .venv/bin/python."""
import glob
from PIL import Image
fs = sorted(glob.glob('build/shot_*.png'))
cols = 5; w, h = 200, 240
W = Image.new('RGB', (cols * w, ((len(fs) + cols - 1) // cols) * h))
for i, f in enumerate(fs):
    W.paste(Image.open(f).convert('RGB').resize((w, h)), ((i % cols) * w, (i // cols) * h))
W.save('build/sheet.png'); print(len(fs), 'frames:', ' '.join(f[-10:-4] for f in fs))
