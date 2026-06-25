# Breadcrumbs for OBS

Drop timestamped markers into your recording with a hotkey, so you can find the
moments that matter when you edit later — instead of scrubbing through hours of
footage.

Built for long, unbroken recordings (game runs, streams). Markers are written to
a plain-text sidecar file next to the recording, which means it works with
**any container — including MKV** (OBS's built-in chapter markers only support
MP4/MOV).

## How it works

- Define up to **5 marker categories** (e.g. `Boss`, `Death`, `Loot`, `Funny`)
  in **Tools → Breadcrumbs…**.
- Bind a key to each category in **Settings → Hotkeys** (search for
  *"Breadcrumb"*).
- While recording, press a category's hotkey to append a line to a `.txt` file
  next to the recording:

  ```
  00:12:43 - Boss
  00:41:09 - Death
  01:58:21 - Funny
  ```

- The timestamp is the **position inside the video file** (frames ÷ FPS), so it
  stays correct across pauses and lands exactly where your editor's playhead
  goes when you scrub to it.

The sidecar shares the recording's name: `2026-06-25_run.mkv` →
`2026-06-25_run.txt`.

## Building

The project uses the standard OBS plugin build system (CMake + the official
template's GitHub Actions CI), producing artifacts for Windows, macOS, and
Linux.

### Local (Linux, against installed libobs)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_FRONTEND_API=ON -DENABLE_QT=ON
cmake --build build
```

### Windows / macOS / packaged builds

Use the bundled CMake presets (they download the matching OBS + Qt deps listed
in `buildspec.json`), or just push to GitHub and let the CI in
`.github/workflows` build and package all three platforms.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
