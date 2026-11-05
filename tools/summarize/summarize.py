#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "pillow"]
# ///
"""Make a chaptered, trimmed summary of a long scope recording, plus a visual outline page.

Workflow (see README.md):
  summarize.py extract REC.mp4          # keyframe samples, activity metrics, per-minute contact sheets
  summarize.py sheet REC.mp4 1:05 1:32  # closer look at a stretch (contact sheet)
  summarize.py frame REC.mp4 1:20:40    # full-resolution frames
  (write work/chapters.tsv)
  summarize.py build REC.mp4            # select segments, cut at keyframes, chapters, captions, outline
  summarize.py share REC.mp4            # hard-link the four shareable files into a share folder

Nothing is re-encoded: segments are cut on the encoder's keyframes (closed GOPs) and stream-copied.
"""
import argparse, base64, bisect, csv, datetime, html, json, os, shutil, subprocess, sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

GRAY_W, GRAY_H = 320, 200          # analysis resolution
THUMB_W = 480                      # thumbnail width (height keeps aspect)


# --- paths and small helpers -------------------------------------------------------------------

class Paths:
    def __init__(self, rec: Path, out: Path | None):
        self.rec = rec.resolve()
        self.stem = self.rec.stem
        self.root = (out or self.rec.parent / f"summary_{self.stem}").resolve()
        self.work = self.root / "work"
        self.thumbs = self.work / "thumbs"
        self.sheets = self.work / "sheets"
        self.frames = self.work / "frames"
        self.base = self.root / f"{self.stem}_summary"   # .mp4 / .srt / .vtt


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def parse_time(s: str) -> float:
    """'h:mm:ss', 'm:ss' (minutes may exceed 59) or plain seconds."""
    parts = [float(p) for p in s.strip().split(":")]
    secs = 0.0
    for p in parts:
        secs = secs * 60 + p
    return secs


def hms(s: float) -> str:
    s = int(round(s))
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"


def short(s: float) -> str:
    s = int(round(s))
    h, m, x = s // 3600, s % 3600 // 60, s % 60
    return f"{h}:{m:02d}:{x:02d}" if h else f"{m}:{x:02d}"


def srt_time(s: float) -> str:
    ms = int(round(s * 1000))
    return f"{ms // 3600000:02d}:{ms % 3600000 // 60000:02d}:{ms % 60000 // 1000:02d},{ms % 1000:03d}"


def load_keyframes(p: Paths) -> np.ndarray:
    return np.loadtxt(p.work / "ktimes.txt", ndmin=1)


def thumb_for(p: Paths, kt: np.ndarray, t: float) -> Path:
    i = min(int(np.searchsorted(kt, t)), len(kt) - 1)
    return p.thumbs / f"{i + 1:06d}.jpg"


def contact_sheet(tiles, out: Path, cols=8, tile_w=240):
    """tiles: list of (image path, label). Writes one JPEG."""
    if not tiles:
        return
    first = Image.open(tiles[0][0])
    tile_h = round(tile_w * first.height / first.width)
    font = ImageFont.load_default(size=14)
    lab = 20
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tile_w + 4), rows * (tile_h + lab + 4)), "white")
    d = ImageDraw.Draw(sheet)
    for k, (img, label) in enumerate(tiles):
        x, y = k % cols * (tile_w + 4) + 2, k // cols * (tile_h + lab + 4) + 2
        sheet.paste(Image.open(img).convert("RGB").resize((tile_w, tile_h)), (x, y))
        w = d.textlength(label, font=font)
        d.text((x + (tile_w - w) / 2, y + tile_h + 2), label, fill="black", font=font)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out, quality=88)
    print(out)


# --- extract -----------------------------------------------------------------------------------

def cmd_extract(p: Paths, a):
    p.thumbs.mkdir(parents=True, exist_ok=True)
    if any(p.thumbs.iterdir()):
        sys.exit(f"{p.thumbs} is not empty; extraction was already run (delete work/ to redo it).")
    # Decode keyframes only (one every 2 s with the recorder's settings). -fps_mode must be given
    # for each output, or the image output duplicates frames up to the stream's frame rate.
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-skip_frame", "nokey", "-i", p.rec, "-an",
         "-filter_complex",
         f"[0:v]split=2[a][b];[a]scale={GRAY_W}:{GRAY_H},format=gray[g];[b]scale={THUMB_W}:-2[c]",
         "-map", "[g]", "-fps_mode", "passthrough", "-f", "rawvideo", p.work / "gray.raw",
         "-map", "[c]", "-fps_mode", "passthrough", "-q:v", "4", p.thumbs / "%06d.jpg"])
    # Keyframe times from the packet index (no decoding).
    out = run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=pts_time,flags",
               "-of", "csv=p=0", p.rec], capture_output=True, text=True).stdout
    kt = sorted(float(l.split(",")[0]) for l in out.splitlines() if "K" in l.split(",")[1])
    n = (p.work / "gray.raw").stat().st_size // (GRAY_W * GRAY_H)
    nthumbs = len(list(p.thumbs.glob("*.jpg")))
    if not (len(kt) == n == nthumbs):
        sys.exit(f"keyframe count mismatch: {len(kt)} packets, {n} samples, {nthumbs} thumbnails")
    np.savetxt(p.work / "ktimes.txt", kt, fmt="%.6f")

    g = np.fromfile(p.work / "gray.raw", np.uint8).reshape(n, GRAY_H, GRAY_W).astype(np.float32)
    bright = g.reshape(n, -1).mean(1)
    lap = 4 * g[:, 1:-1, 1:-1] - g[:, :-2, 1:-1] - g[:, 2:, 1:-1] - g[:, 1:-1, :-2] - g[:, 1:-1, 2:]
    sharp = lap.reshape(n, -1).var(1)
    # Motion between consecutive keyframes on 8x8 block means: sensor noise averages out, so dim
    # high-gain scenes don't read as busy. 'local' (4x4 blocks, 99.5th percentile) catches small
    # movements, such as threading suture, that a whole-frame mean misses.
    b8 = g.reshape(n, GRAY_H // 8, 8, GRAY_W // 8, 8).mean((2, 4))
    motion = np.r_[0, np.abs(np.diff(b8, axis=0)).reshape(n - 1, -1).mean(1)]
    b4 = g.reshape(n, GRAY_H // 4, 4, GRAY_W // 4, 4).mean((2, 4))
    local = np.r_[0, np.percentile(np.abs(np.diff(b4, axis=0)).reshape(n - 1, -1), 99.5, axis=1)]
    np.savetxt(p.work / "metrics.csv", np.c_[kt, bright, sharp, motion, local], delimiter=",",
               fmt=["%.3f", "%.1f", "%.1f", "%.2f", "%.1f"], header="t,bright,sharp,motion,local", comments="")
    (p.work / "gray.raw").unlink()   # 0.4 GB; everything needed is in metrics.csv

    kt = np.array(kt)
    total_min = int(kt[-1] // 60) + 1
    tiles = [(thumb_for(p, kt, m * 60 + 30), hms(m * 60 + 30)) for m in range(total_min)]
    for k in range(0, len(tiles), 40):
        contact_sheet(tiles[k:k + 40], p.sheets / f"overview_{k // 40:02d}.jpg")
    print("\nMedian motion per minute (idle camera < 2, active surgery ~5-25):")
    for m in range(total_min):
        s = (kt >= m * 60) & (kt < m * 60 + 60)
        print(f"{hms(m * 60)[:-3]:>5} {np.median(motion[s]):5.1f}", end="\n" if m % 8 == 7 else "  |")
    print()
    ch = p.work / "chapters.tsv"
    if not ch.exists():
        ch.write_text("# start<TAB>end<TAB>title[<TAB>full]   times as h:mm:ss or m:ss\n"
                      "# 'full' keeps the whole chapter uncut; otherwise segments are selected.\n"
                      "# Gaps between chapters are cut.\n")
    print(f"\nNext: look at {p.sheets}/overview_*.jpg, then write {ch}")


# --- inspection ----------------------------------------------------------------------------------

def cmd_sheet(p: Paths, a):
    start, end = parse_time(a.start), parse_time(a.end)
    name = a.name or f"sheet_{hms(start).replace(':', '')}_{hms(end).replace(':', '')}"
    if a.step >= 2:   # keyframe thumbnails are enough
        kt = load_keyframes(p)
        times = np.arange(start, end, a.step)
        tiles = [(thumb_for(p, kt, t), hms(t)) for t in times]
    else:             # finer than the keyframe spacing: decode the video
        tmp = p.frames / name
        tmp.mkdir(parents=True, exist_ok=True)
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", start, "-i", p.rec, "-t", end - start,
             "-vf", f"fps=1/{a.step},scale=640:-2", "-start_number", "0", tmp / "f_%04d.jpg"])
        tiles = [(f, hms(start + k * a.step)) for k, f in enumerate(sorted(tmp.glob("f_*.jpg")))]
    contact_sheet(tiles, p.sheets / f"{name}.jpg", cols=a.cols, tile_w=a.width)


def cmd_frame(p: Paths, a):
    p.frames.mkdir(parents=True, exist_ok=True)
    for ts in a.times:
        t = parse_time(ts)
        out = p.frames / f"frame_{hms(t).replace(':', '')}.jpg"
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", t, "-i", p.rec, "-frames:v", "1",
             "-vf", f"scale={a.width}:-2", "-q:v", "3", out])
        print(out)


# --- build ---------------------------------------------------------------------------------------

def read_chapters(p: Paths):
    chapters = []
    for line in (p.work / "chapters.tsv").read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        f = line.split("\t")
        if len(f) < 3:
            sys.exit(f"chapters.tsv: need start<TAB>end<TAB>title: {line!r}")
        a, b = parse_time(f[0]), parse_time(f[1])
        if b <= a:
            sys.exit(f"chapters.tsv: end before start: {line!r}")
        chapters.append({"name": f[2].strip(), "orig": [a, b], "full": len(f) > 3 and f[3].strip() == "full"})
    for x, y in zip(chapters, chapters[1:]):
        if y["orig"][0] < x["orig"][1]:
            sys.exit(f"chapters.tsv: {y['name']!r} overlaps {x['name']!r}")
    if not chapters:
        sys.exit("chapters.tsv has no chapters")
    return chapters


def select(chapters, d, frac, win, lo, hi):
    """Per chapter: a budget of frac x duration (clamped to [lo, hi] s), split into windows of ~win s,
    each taken from its own equal slice of the chapter, so the summary shows how the phase progresses."""
    t, bright, sharp, motion = d["t"], d["bright"], d["sharp"], d["motion"]
    for c in chapters:
        a, b = c["orig"]
        idx = np.where((t >= a) & (t < b))[0]
        if len(idx) < 2:
            sys.exit(f"chapter {c['name']!r} has no keyframes")
        end_i = min(idx[-1] + 1, len(t) - 1)
        if c["full"]:
            c["segments"] = [[float(t[idx[0]]), float(t[end_i])]]
            continue
        # Score: activity, minus idle stretches, overexposure (lamp/hand glare), camera bumps, and
        # frames well below the chapter's typical focus (out-of-focus hands in front of the lens).
        med_sharp = np.median(sharp[idx])
        q = np.clip(motion, 0, 15) - 8 * (motion < 2) - 15 * (bright > 175) - 10 * (motion > 35) \
            - 6 * (sharp < 0.5 * med_sharp)
        budget = min(max(lo, frac * (b - a)), hi)
        nb = max(1, round(budget / win))
        edges = np.linspace(a, b, nb + 1)
        segs = []
        for k in range(nb):
            cand = idx[(t[idx] >= edges[k]) & (t[idx] + win <= edges[k + 1] + 1)]
            best = None
            for i in cand:
                j = int(np.searchsorted(t, t[i] + win))
                sc = q[i:j].mean()
                if best is None or sc > best[0]:
                    best = (sc, i, min(j, len(t) - 1))
            if best:
                segs.append([float(t[best[1]]), float(t[best[2]])])
        merged = []
        for s in segs:
            if merged and s[0] <= merged[-1][1] + 0.01:
                merged[-1][1] = s[1]
            else:
                merged.append(s)
        c["segments"] = merged


def ffmeta_escape(s: str) -> str:
    return "".join("\\" + ch if ch in "=;#\\\n" else ch for ch in s)


def recording_title(p: Paths) -> tuple[str, str]:
    side = p.rec.with_suffix(".json")
    try:
        started = json.loads(side.read_text())["recording"]["started"]
        dt = datetime.datetime.fromisoformat(started)
        return f"{dt.day} {dt:%b %Y}, {dt:%H:%M}", f"{dt:%Y-%m-%d %H:%M}"
    except (OSError, KeyError, ValueError):
        return p.stem, p.stem


def cmd_build(p: Paths, a):
    d = np.genfromtxt(p.work / "metrics.csv", delimiter=",", names=True)
    kt = d["t"]
    chapters = read_chapters(p)
    select(chapters, d, a.frac, a.window, a.min, a.max)

    # Summary timeline, chapters, and per-second captions with the original recording time.
    pos, segments, cues = 0.0, [], []
    for ci, c in enumerate(chapters):
        c["summary"] = [pos, None]
        for s, e in c["segments"]:
            segments.append({"chapter": ci, "summary_start": pos, "orig_start": s, "orig_end": e})
            for k in range(int(np.ceil(e - s - 1e-6))):
                cues.append((pos + k, pos + min(k + 1, e - s), f"C{ci + 1} · {c['name']} · original {hms(s + k)}"))
            pos += e - s
        c["summary"][1] = pos
    long_title, short_title = recording_title(p)
    (p.work / "plan.json").write_text(json.dumps({"chapters": chapters, "segments": segments}, indent=1))
    meta = [";FFMETADATA1", f"title={ffmeta_escape('Surgery summary, ' + short_title + ' recording')}"]
    for ci, c in enumerate(chapters):
        meta += ["[CHAPTER]", "TIMEBASE=1/1000", f"START={int(c['summary'][0] * 1000)}",
                 f"END={int(c['summary'][1] * 1000)}", f"title={ffmeta_escape(f'{ci + 1}. ' + c['name'])}"]
    (p.work / "chapters.ffmeta").write_text("\n".join(meta) + "\n")
    srt = Path(f"{p.base}.srt")
    srt.write_text("".join(f"{n}\n{srt_time(x)} --> {srt_time(y)}\n{txt}\n\n" for n, (x, y, txt) in enumerate(cues, 1)))
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", srt, f"{p.base}.vtt"])

    review_sheets(p, kt, segments)

    # Cut. The segment muxer splits exactly at keyframe packets; its split times count from the
    # stream's first timestamp (0.1 s with the recorder), hence the t0 offset. Unwanted pieces are
    # written too, so this needs free space about the size of the recording, briefly.
    seg = p.work / "seg"
    shutil.rmtree(seg, ignore_errors=True)
    seg.mkdir()
    t0 = kt[0]
    bounds = sorted({round(x, 3) for s in segments for x in (s["orig_start"], s["orig_end"])})
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", p.rec, "-map", "0:v", "-c", "copy",
         "-f", "segment", "-segment_times", ",".join(f"{b - t0 - 0.02:.3f}" for b in bounds),
         "-segment_format", "mp4", "-reset_timestamps", "1",
         "-segment_list", p.work / "seglist.csv", "-segment_list_type", "csv", seg / "%04d.mp4"])
    pieces = list(csv.reader(open(p.work / "seglist.csv")))
    with open(p.work / "join.txt", "w") as j:
        for s in segments:
            r = min(pieces, key=lambda r: abs(float(r[1]) - s["orig_start"]))
            want = s["orig_end"] - s["orig_start"]
            if abs(float(r[1]) - s["orig_start"]) > 0.05 or abs(float(r[2]) - float(r[1]) - want) > 0.05:
                sys.exit(f"split piece {r} does not match segment {s}")
            j.write(f"file 'seg/{r[0]}'\n")
    joined = p.work / "joined.mp4"
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0",
         "-i", p.work / "join.txt", "-c", "copy", joined])
    # Chapters go in the MP4; captions stay as sidecar files: ffmpeg's MP4 muxer corrupts the
    # chapter list when it also writes a mov_text subtitle track.
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", joined, "-i", p.work / "chapters.ffmeta",
         "-map", "0:v", "-map_metadata", "1", "-map_chapters", "1", "-c", "copy", "-movflags", "+faststart",
         f"{p.base}.mp4"])
    if not a.keep_intermediates:
        shutil.rmtree(seg)
        joined.unlink()

    write_outline(p, kt, chapters, segments, long_title)
    verify(p, chapters, pos)


def review_sheets(p: Paths, kt, segments):
    tiles = []
    for s in segments:
        x, y = s["orig_start"], s["orig_end"]
        for t in (x + 1, (x + y) / 2, y - 2):
            tiles.append((thumb_for(p, kt, t), f"C{s['chapter'] + 1} {hms(t)}"))
    for k in range(0, len(tiles), 90):
        contact_sheet(tiles[k:k + 90], p.sheets / f"selected_{k // 90:02d}.jpg", cols=6)


def verify(p: Paths, chapters, length):
    out = run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=pts_time",
               "-of", "csv=p=0", f"{p.base}.mp4"], capture_output=True, text=True).stdout
    pts = np.sort(np.array([float(x) for x in out.split()]))
    gaps = int(np.sum((np.diff(pts) <= 0) | (np.diff(pts) > 0.1)))
    print(f"\n{p.base}.mp4: {short(length)} from {short(chapters[-1]['orig'][1])} of chapters, "
          f"{len(pts)} frames, {gaps} timestamp gaps")
    for ci, c in enumerate(chapters):
        kept = c["summary"][1] - c["summary"][0]
        print(f"{ci + 1:3d}. {short(c['summary'][0]):>6}  orig {hms(c['orig'][0])}-{hms(c['orig'][1])}"
              f"  kept {short(kept):>5}{'  (whole)' if c['full'] else ''}  {c['name']}")
    if gaps:
        sys.exit("timestamp gaps found; check the cut")


# --- outline page --------------------------------------------------------------------------------

def write_outline(p: Paths, kt, chapters, segments, title):
    total = float(kt[-1]) + 2
    hues = [(20 + i * 137) % 360 for i in range(len(chapters))]   # golden-angle spacing
    def data_uri(t):
        return "data:image/jpeg;base64," + base64.b64encode(thumb_for(p, kt, t).read_bytes()).decode()
    bars = "".join(
        f'<button class="seg" style="left:{s["orig_start"] / total * 100:.3f}%;'
        f'width:{max((s["orig_end"] - s["orig_start"]) / total * 100, 0.35):.3f}%;--h:{hues[s["chapter"]]}" '
        f'data-t="{s["summary_start"]:.2f}" title="Chapter {s["chapter"] + 1} · original {short(s["orig_start"])}"></button>'
        for s in segments)
    ticks = "".join(f'<span class="tick" style="left:{m * 60 / total * 100:.2f}%">{m // 60}h{m % 60:02d}</span>'
                    for m in range(0, int(total / 60) + 1, 30))
    cards = []
    for ci, c in enumerate(chapters):
        (a, b), (s0, s1) = c["orig"], c["summary"]
        shots = []
        for s in (s for s in segments if s["chapter"] == ci):
            L = s["orig_end"] - s["orig_start"]
            offs = [L / 2] if L <= 60 else list(range(15, int(L), 45))   # long segments: one every 45 s
            shots += [(s["summary_start"] + o, s["orig_start"] + o) for o in offs]
        strip = "".join(f'<button class="shot" data-t="{st:.2f}"><img loading="lazy" alt="" src="{data_uri(ot)}">'
                        f'<span>{short(ot)}</span></button>' for st, ot in shots)
        cards.append(
            f'<li class="chap" style="--h:{hues[ci]}" data-t="{s0:.2f}"><button class="head" data-t="{s0:.2f}">'
            f'<span class="num">{ci + 1}</span><span class="ttl">{html.escape(c["name"])}</span>'
            f'<span class="meta">summary {short(s0)}–{short(s1)} · original {short(a)}–{short(b)} · '
            f'kept {short(s1 - s0)} of {short(b - a)}</span></button><div class="strip">{strip}</div></li>')
    name = p.base.name
    page = OUTLINE.format(title=html.escape(title), length=short(chapters[-1]["summary"][1]), total=short(total),
                          nch=len(chapters), nseg=len(segments), video=f"{name}.mp4", vtt=f"{name}.vtt",
                          first=html.escape(chapters[0]["name"]), bars=bars, ticks=ticks, cards="".join(cards),
                          source=html.escape(p.rec.name))
    (p.root / "outline.html").write_text(page)
    print(p.root / "outline.html")


OUTLINE = """<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Surgery Summary Outline</title><style>
:root{{--bg:#f6f5f2;--fg:#1c1c1a;--mut:#6b6a66;--card:#fff;--line:#dedcd6;--acc:#2f6fd0}}
@media (prefers-color-scheme:dark){{:root:not([data-theme="light"]){{--bg:#141413;--fg:#ecebe7;--mut:#9a9893;--card:#1e1e1c;--line:#34332f;--acc:#7aa7ff}}}}
:root[data-theme="dark"]{{--bg:#141413;--fg:#ecebe7;--mut:#9a9893;--card:#1e1e1c;--line:#34332f;--acc:#7aa7ff}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}}
main{{max-width:1180px;margin:0 auto;padding:20px 16px 60px}}
h1{{font-size:22px;margin:0 0 4px}} .sub{{color:var(--mut);margin:0 0 16px}}
.layout{{display:grid;grid-template-columns:minmax(0,1.5fr) minmax(0,1fr);gap:20px;align-items:start}}
@media (max-width:900px){{.layout{{grid-template-columns:1fr}}}}
.player{{position:sticky;top:12px}} video{{width:100%;background:#000;border-radius:8px;display:block}}
.now{{margin:8px 0 0;font-weight:600}} .keys{{color:var(--mut);font-size:13px;margin-top:6px}}
kbd{{border:1px solid var(--line);border-bottom-width:2px;border-radius:4px;padding:0 5px;font:12px ui-monospace,monospace;background:var(--card)}}
.tl{{margin:18px 0 6px}} .tl h2,.list h2{{font-size:14px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin:0 0 8px}}
.track{{position:relative;height:30px;background:repeating-linear-gradient(135deg,var(--line) 0 4px,transparent 4px 9px);border-radius:5px;border:1px solid var(--line)}}
.seg{{position:absolute;top:0;bottom:0;border:0;padding:0;background:hsl(var(--h) 60% 50%);cursor:pointer}}
.seg:focus-visible,.shot:focus-visible,.head:focus-visible{{outline:3px solid var(--acc);outline-offset:2px}}
.ticks{{position:relative;height:18px;font-size:11px;color:var(--mut)}} .tick{{position:absolute;transform:translateX(-50%)}}
.legend{{font-size:12px;color:var(--mut)}}
.list{{min-width:0}} .list ol{{list-style:none;margin:0;padding:0;display:grid;grid-template-columns:minmax(0,1fr);gap:10px}}
.chap{{min-width:0;background:var(--card);border:1px solid var(--line);border-left:5px solid hsl(var(--h) 60% 50%);border-radius:8px;padding:8px 10px}}
.chap.active{{box-shadow:0 0 0 2px var(--acc)}}
.head{{all:unset;display:grid;grid-template-columns:auto 1fr;gap:0 8px;cursor:pointer;width:100%}}
.num{{grid-row:span 2;font-weight:700;font-size:18px;min-width:1.6em;color:hsl(var(--h) 55% 45%)}}
.ttl{{font-weight:600}} .meta{{font-size:12px;color:var(--mut)}}
.strip{{display:flex;gap:6px;overflow-x:auto;margin-top:8px;padding-bottom:2px}}
.shot{{all:unset;cursor:pointer;flex:0 0 auto;width:132px;position:relative}}
.shot img{{width:132px;height:82px;object-fit:cover;border-radius:4px;display:block}}
.shot span{{position:absolute;left:4px;bottom:4px;background:#000a;color:#fff;font-size:11px;padding:0 4px;border-radius:3px}}
.foot{{color:var(--mut);font-size:13px;margin-top:24px;max-width:75ch}}
</style></head><body><main>
<h1>Surgery summary — {title}</h1>
<p class="sub">{length} summary of a {total} recording · {nch} chapters · {nseg} segments. Stream-copied from the original: no re-encoding.</p>
<div class="layout">
<section class="player"><video id="v" controls preload="metadata" src="{video}"><track kind="captions" src="{vtt}" srclang="en" label="Original recording time" default></video>
<p class="now" id="now">1. {first}</p>
<p class="keys"><kbd>Space</kbd> play/pause · <kbd>n</kbd>/<kbd>p</kbd> next/previous chapter · <kbd>1</kbd>–<kbd>9</kbd> jump to chapter · <kbd>←</kbd>/<kbd>→</kbd> ±5 s · <kbd>c</kbd> toggle original-time captions · <kbd>Tab</kbd> to move through thumbnails, <kbd>Enter</kbd> to jump</p>
<div class="tl"><h2>What was kept from the full recording</h2>
<div class="track">{bars}</div><div class="ticks">{ticks}</div>
<div class="legend">Coloured: kept, by chapter. Hatched: cut — idle camera, repositioning, and the parts of each phase not chosen.</div></div>
</section>
<section class="list"><h2>Chapters</h2><ol>{cards}</ol></section>
</div>
<p class="foot">Captions show the original recording time of every frame, so anything in the summary can be found in the full file <code>{source}</code>. In mpv, chapters are on <kbd>PgUp</kbd>/<kbd>PgDn</kbd> and captions on <kbd>v</kbd>.</p>
</main><script>
const v=document.getElementById('v'),now=document.getElementById('now');
const chaps=[...document.querySelectorAll('.chap')],starts=chaps.map(c=>+c.dataset.t);
function seek(t){{v.currentTime=t+0.01;v.play().catch(()=>{{}});}}
document.querySelectorAll('button[data-t]').forEach(el=>el.addEventListener('click',()=>seek(+el.dataset.t)));
function cur(){{let i=0;starts.forEach((s,k)=>{{if(v.currentTime>=s-0.05)i=k}});return i}}
v.addEventListener('timeupdate',()=>{{const i=cur();chaps.forEach((c,k)=>c.classList.toggle('active',k===i));now.textContent=chaps[i].querySelector('.num').textContent+'. '+chaps[i].querySelector('.ttl').textContent;}});
document.addEventListener('keydown',e=>{{if(e.ctrlKey||e.metaKey||e.altKey)return;const i=cur();
 if(e.key==='n'&&i<starts.length-1)seek(starts[i+1]);
 else if(e.key==='p')seek(v.currentTime-starts[i]>3||i===0?starts[i]:starts[i-1]);
 else if(/^[1-9]$/.test(e.key)&&+e.key<=starts.length)seek(starts[+e.key-1]);
 else if(e.key==='c'){{const tr=v.textTracks[0];tr.mode=tr.mode==='showing'?'hidden':'showing'}}
 else if(e.key===' '&&e.target===document.body){{e.preventDefault();v.paused?v.play():v.pause()}}
 else if(e.key==='ArrowRight'&&e.target===document.body)v.currentTime+=5;
 else if(e.key==='ArrowLeft'&&e.target===document.body)v.currentTime-=5;}});
</script></body></html>"""


# --- share ---------------------------------------------------------------------------------------

def cmd_share(p: Paths, a):
    dest = (a.dest or p.rec.parent / "share") / p.root.name
    dest.mkdir(parents=True, exist_ok=True)
    for f in (Path(f"{p.base}.mp4"), Path(f"{p.base}.srt"), Path(f"{p.base}.vtt"), p.root / "outline.html"):
        target = dest / f.name
        target.unlink(missing_ok=True)
        try:
            os.link(f, target)            # same disk: no extra space, follows rebuilds
        except OSError:
            shutil.copy2(f, target)       # different disk: plain copy
        print(target)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, help="summary folder (default: summary_<recording> next to it)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    def add(name, fn, help):
        sp = sub.add_parser(name, help=help)
        sp.add_argument("recording", type=Path)
        sp.set_defaults(fn=fn)
        return sp
    add("extract", cmd_extract, "keyframe samples, metrics, overview contact sheets")
    sp = add("sheet", cmd_sheet, "contact sheet of a time range")
    sp.add_argument("start"); sp.add_argument("end")
    sp.add_argument("--step", type=float, default=10, help="seconds between tiles (default 10; <2 decodes the video)")
    sp.add_argument("--cols", type=int, default=8); sp.add_argument("--width", type=int, default=240)
    sp.add_argument("--name")
    sp = add("frame", cmd_frame, "full-resolution frames at given times")
    sp.add_argument("times", nargs="+"); sp.add_argument("--width", type=int, default=1440)
    sp = add("build", cmd_build, "select segments, cut, chapters, captions, outline")
    sp.add_argument("--frac", type=float, default=0.15, help="fraction of each chapter to keep (default 0.15)")
    sp.add_argument("--window", type=float, default=40, help="segment length in s (default 40)")
    sp.add_argument("--min", type=float, default=60, help="minimum kept per chapter in s (default 60)")
    sp.add_argument("--max", type=float, default=240, help="maximum kept per chapter in s (default 240)")
    sp.add_argument("--keep-intermediates", action="store_true", help="keep work/seg and work/joined.mp4")
    sp = add("share", cmd_share, "hard-link the shareable files into a share folder")
    sp.add_argument("--dest", type=Path, help="share folder (default: share/ next to the recording)")
    a = ap.parse_args()
    if not a.recording.is_file():
        sys.exit(f"no such recording: {a.recording}")
    a.fn(Paths(a.recording, a.dir), a)


if __name__ == "__main__":
    main()
