"""Check the mod's IMA ADPCM decoder (src/portal_adpcm.h) against libsndfile.

Copies a spread of built voice lines (assets/voice) to a temp folder, decodes them with
bin/test_adpcm.exe (build/tadpcm.bat builds it), and requires every sample to equal libsndfile's
decode, up to the fact chunk's sample count. Run with a Python that has soundfile and numpy.
"""
import glob, os, random, shutil, subprocess, sys, tempfile
import numpy as np, soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
files = sorted(glob.glob(os.path.join(ROOT, 'assets', 'voice', '*', '*', '*.wav')))
random.seed(1)
pick = random.sample(files, min(60, len(files)))
work = tempfile.mkdtemp()
local = []
for i, p in enumerate(pick):
    local.append(os.path.join(work, f'{i}.wav'))
    shutil.copy(p, local[-1])
subprocess.run([os.path.join(ROOT, 'bin', 'test_adpcm.exe'), *local], check=True)
bad = 0
for p, src in zip(local, pick):
    ours, rate = sf.read(p + '.pcm.wav', dtype='int16')
    theirs, rate2 = sf.read(p, dtype='int16')
    if rate != rate2 or len(ours) > len(theirs) or len(theirs) - len(ours) >= 1017 or \
            not np.array_equal(ours, theirs[:len(ours)]):
        bad += 1
        print('MISMATCH', src)
shutil.rmtree(work)
print(f'{len(pick) - bad}/{len(pick)} match libsndfile')
sys.exit(bad != 0)
