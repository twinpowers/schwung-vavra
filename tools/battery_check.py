"""Score the audio battery captured by tests/audio_battery.sh.

The module is compared against an OFFLINE render of the same engine driven
directly -- no fork, no ring, no gain -- so what is under test is this
module's own audio path and its preset/part handling, not the emulator.

Identity is spectral, not sample-exact, because the emulator is not
reproducible: mqLib runs the 68k in m_ucThread and the DSP in a DSPThread and
their interleaving moves the output. Two renders of the SAME preset differ by
1.4-5.0 on this metric, so a capture must match its own reference well inside
that and beat every other reference by 2x."""
import numpy as np, wave, sys, os

BATTERY = os.environ.get("VAVRA_BATTERY_DIR", "battery")

def load(p):
    with wave.open(p) as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1,2).astype(float)

def feature(path, t0=1.0, t1=3.0):
    """Log-band spectrum of a FIXED note-on-aligned window, level-normalised.

    Both the module capture and the offline render begin at note-on, so the
    same wall-clock window compares the same phase of the patch. Picking the
    'loudest window' of each instead compares an evolving patch against a
    different part of itself and reports a mismatch."""
    m = load(path).mean(axis=1)
    a, b = int(t0*44100), int(t1*44100)
    if len(m) < b: raise SystemExit(f"{path}: shorter than {t1}s")
    seg = m[a:b] * np.hanning(b-a)
    spec = np.abs(np.fft.rfft(seg)); freqs = np.fft.rfftfreq(b-a, 1/44100)
    edges = np.logspace(np.log10(40), np.log10(16000), 41)
    bands = [spec[(freqs>=lo)&(freqs<hi)].mean() if ((freqs>=lo)&(freqs<hi)).any() else 1e-9
             for lo,hi in zip(edges[:-1], edges[1:])]
    b_db = 20*np.log10(np.maximum(np.array(bands), 1e-9))
    return b_db - b_db.max()

def rms(path): 
    a = load(path); return float(np.sqrt((a**2).mean()))

NAMES = {0:"A1 LosAngeles2019", 16:"A17 11KHz Solo", 100:"B1 Jazz Percssn",
         200:"C1 The Beginning", 299:"C100 Init Sound"}
refs = {p: feature(f"{BATTERY}/R{p}.wav") for p in NAMES}
d = lambda a,b: float(np.abs(a-b).mean())

def identify(path):
    f = feature(path)
    row = {p: d(f, refs[p]) for p in refs}
    best = min(row, key=row.get)
    return best, row[best], sorted(row.values())[1], row

fails = []
print("=== SINGLE MODE: each capture must match its OWN preset, by 2x ===")
for p in NAMES:
    best, dist, second, _ = identify(f"{BATTERY}/single_{p}.wav")
    ok = best == p and second > dist*2
    if not ok: fails.append(f"single preset {p}")
    print(f"  {NAMES[p]:<18} -> {NAMES[best]:<18} d={dist:5.2f} next={second:5.2f}  {'PASS' if ok else 'FAIL'}")

print("\n=== MULTI MODE: each channel must play its own part ===")
for f_, expect, label in [("multi_p1",0,"part1=A1 ch1, play ch1"), ("multi_p2",100,"part2=B1 ch2, play ch2"),
                          ("multi_p3",200,"part1=C1 ch1, play ch1"), ("multi_p4",299,"part2=C100 ch2, play ch2")]:
    best, dist, second, _ = identify(f"{BATTERY}/{f_}.wav")
    ok = best == expect and second > dist*2
    if not ok: fails.append(label)
    print(f"  {label:<26} -> {NAMES[best]:<18} d={dist:5.2f} next={second:5.2f}  {'PASS' if ok else 'FAIL'}")

print("\n=== MULTI MODE: volume and layering ===")
muted, audible = rms(f"{BATTERY}/multi_muted.wav"), rms(f"{BATTERY}/multi_p2.wav")
ok = muted < audible*0.02
if not ok: fails.append("part volume 0")
print(f"  part2 volume=0 on its channel: rms {muted:.1f} vs {audible:.1f} ({muted/audible*100:.2f}%)  {'PASS' if ok else 'FAIL'}")

layer, alone = rms(f"{BATTERY}/multi_layered.wav"), rms(f"{BATTERY}/multi_p1.wav")
best, dist, second, row = identify(f"{BATTERY}/multi_layered.wav")
ok = layer > alone*1.05
if not ok: fails.append("layering")
print(f"  both parts on ch1: rms {layer:.1f} vs {alone:.1f} alone (+{(layer/alone-1)*100:.0f}%)  {'PASS' if ok else 'FAIL'}")
print("     distances: " + "  ".join(f"{NAMES[p][:8]}={row[p]:.1f}" for p in row))

print(f"\n{'ALL PASS' if not fails else 'FAILURES: ' + ', '.join(fails)}")
sys.exit(0 if not fails else 1)
