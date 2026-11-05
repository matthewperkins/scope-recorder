# Recording summaries

`summarize.py` turns a long scope recording into a short, chaptered summary video and a visual outline page that people can use to jump through it. Nothing is re-encoded: segments are cut on the encoder's keyframes and stream-copied, so the summary has the original quality.

For each recording it produces, in `summary_<recording>/` next to the recording:

| File | |
|---|---|
| `<recording>_summary.mp4` | The summary, with chapter markers (mpv: PgUp/PgDn) |
| `<recording>_summary.srt` / `.vtt` | Captions showing the original recording time of every frame (mpv: `v`) |
| `outline.html` | Player, a timeline of what was kept, and chapter cards with clickable thumbnails. Keyboard: `n`/`p` chapters, `1`–`9`, `c` captions |
| `work/` | Thumbnails, metrics, contact sheets and `chapters.tsv`; needed only to rebuild |

Needs `ffmpeg`/`ffprobe` and [uv](https://docs.astral.sh/uv/) (the script declares its own Python dependencies).

## Workflow

```bash
S=tools/summarize/summarize.py
R=~/Videos/ScopeRecordings/2026-10-28_15-42-20.mp4

$S extract $R                       # ~1 min per hour of recording
```

This samples every keyframe (one every 2 s), scores brightness, focus and motion, writes per-minute contact sheets to `work/sheets/overview_*.jpg`, and prints the median motion per minute (an idle camera reads below 2).

Look at the overview sheets to find the phases of the procedure. For a closer look:

```bash
$S sheet $R 1:05:00 1:32:00 --step 10     # contact sheet, a tile every 10 s
$S sheet $R 1:20:30 1:21:00 --step 1      # finer than 2 s decodes the video
$S frame $R 1:20:40                       # full-resolution frame
```

Then write `work/chapters.tsv`: one chapter per line, tab-separated, times as `h:mm:ss` or `m:ss`:

```
0:00:00	0:07:20	Dorsal approach: subcutaneous tunnelling
1:10:20	1:11:46	Suture threaded through the Myomatrix perforation	full
1:19:48	1:21:16	Needle-lasso: suture crimped into a 30 G needle	full
```

- Anything between chapters is cut: idle camera, repositioning, failed attempts.
- `full` keeps a chapter whole. Use it for the steps people need to see unbroken.
- Otherwise about 15% of each chapter is kept (at least 1 and at most 4 minutes), as 40-second segments spread across the chapter, chosen for activity and focus. Change this with `--frac`, `--window`, `--min` and `--max`.

```bash
$S build $R      # ~15 s; rerun after any change to chapters.tsv
```

Check `work/sheets/selected_*.jpg` (start, middle and end of every chosen segment). If a segment is poor, for example a hand in front of the lens, move the chapter's boundaries or split the chapter, and rebuild.

```bash
$S share $R      # hard-links the four shareable files into share/summary_<recording>/
```

Collaborators need only those four files, kept together in one folder.

## Notes

- `build` needs free disk space about the size of the recording for a few seconds: ffmpeg's segment muxer writes every piece, and the unused pieces are then deleted.
- Captions are separate files because ffmpeg's MP4 muxer corrupts the chapter list when it also writes a subtitle track.
- The cut points depend on the recorder's closed GOPs (an IDR keyframe every 2 s). Other H.264 files with open GOPs may show a glitch at each cut.
