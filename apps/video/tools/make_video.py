"""Video clip -> apps/video/rickroll.vid for the Video app (1 bit, full resolution below the top bar, deflate).

    python3 apps/video/tools/make_video.py <video> [fps=4] [start_s=43] [dur_s=15] [precrop=636:476:109:2]

Frames are VW x VH, ordered (Bayer) dither so the dot pattern stays put between frames. File: "VID2", u16 w, u16 h,
u16 frames, u16 fps, then per frame u32 length + the frame (MONO_HLSB, 1 = ink) as a zlib stream. Whole frames
deflate smaller than frame deltas here: the dither makes the deltas noisy.
"""
import struct, subprocess, sys, zlib
import numpy as np

VW, VH = 832, 624
B = np.array([[0]])
for _ in range(3):
    B = np.block([[4 * B, 4 * B + 2], [4 * B + 3, 4 * B + 1]])
THRESH = np.tile((B + 0.5) / B.size, (VH // 8, VW // 8))


def frames(video, fps, start, dur, precrop):
    vf = f"crop={precrop},fps={fps},scale={VW}:{VH}:force_original_aspect_ratio=increase,crop={VW}:{VH},format=gray"
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(start), "-t", str(dur), "-i", video, "-vf", vf,
                          "-f", "rawvideo", "-"], check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape((-1, VH, VW))


def main():
    video = sys.argv[1]
    fps = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    start = float(sys.argv[3]) if len(sys.argv) > 3 else 43
    dur = float(sys.argv[4]) if len(sys.argv) > 4 else 15
    crop = sys.argv[5] if len(sys.argv) > 5 else "636:476:109:2"
    g = frames(video, fps, start, dur, crop) / 255.0
    ink = np.clip((g - 0.5) * 1.3 + 0.5, 0, 1) < THRESH  # contrast 1.3: e-ink midtones look washed out
    out = bytearray(b"VID2" + struct.pack("<4H", VW, VH, len(ink), fps))
    for f in ink:
        z = zlib.compress(np.packbits(f, axis=1, bitorder="big").tobytes(), 9)
        out += struct.pack("<I", len(z)) + z
    open("apps/video/rickroll.vid", "wb").write(out)
    print(f"{len(ink)} frames {VW}x{VH} at {fps} fps: {len(out)} bytes ({len(out) // len(ink)} per frame)")


main()
