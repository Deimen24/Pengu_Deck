# Pengu Deck

Two deck DJ software for Linux. Plays anything FFmpeg can decode from your
music folders and streams tracks straight from SoundCloud into a deck.

![Pengu Deck](docs/screenshot.png)

## Features

- Two decks with three band coloured waveforms (bass / mids / highs), a
  zoomed scrolling view you can scratch with the mouse and a track
  overview for seeking
- Automatic BPM and beat grid detection, stored per track, plus tag BPM
- Sync (tempo and beat phase), pitch fader with ±8/16/50 % range, nudge
- Keylock: change tempo without changing key (Rubber Band)
- CDJ style cue, four hot cues and beat-quantised loops (¼ to 16 beats),
  manual in/out loops, loop halving and doubling
- Mixer with trim, three band EQ with kill switches, low/high pass
  filter, channel faders, crossfader with three curves, master limiter
- Headphone cueing: split stereo output (left = master, right = cue) or a
  4 channel audio interface (3/4 = cue), with cue/master mix
- Record the master output to WAV
- Local library with tag reading, caching, search and sorting
- SoundCloud: search, paste track / playlist / artist links, load your
  likes, streams are decoded on the fly (nothing is written to disk)
- Drag and drop from the library, from your file manager, or open files
  from the command line
- Keyboard shortcuts for both decks (menu → Keyboard shortcuts)
- PulseAudio / PipeWire, ALSA and JACK output

## Installing

### CachyOS / Arch Linux

```sh
git clone https://github.com/deimen24/Pengu_Deck.git
cd Pengu_Deck/packaging/arch
makepkg -si
```

The package depends on `gtk4`, `ffmpeg`, `rubberband`, `json-glib`,
`curl` and `libpulse`; `makepkg` pulls them in. CachyOS ships PipeWire,
so the "Automatic" backend uses `pipewire-pulse`. For lower latency
install `pipewire-jack` and pick the JACK backend in Preferences.

### Fedora

```sh
sudo dnf install gcc meson ninja-build gtk4-devel json-glib-devel \
    libcurl-devel rubberband-devel pulseaudio-libs-devel alsa-lib-devel \
    ffmpeg-devel      # from RPM Fusion for MP3/AAC
meson setup build && meson compile -C build && sudo meson install -C build
```

Or build an RPM with `rpmbuild -bb packaging/fedora/pengu-deck.spec`.

### Ubuntu / Debian

```sh
sudo apt install meson ninja-build libgtk-4-dev libjson-glib-dev \
    libcurl4-openssl-dev libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev librubberband-dev libpulse-dev libasound2-dev
meson setup build && meson compile -C build && sudo meson install -C build
```

Or build a `.deb` with `dpkg-buildpackage -us -uc -b`.

### Flatpak (any distribution)

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.deimen24.PenguDeck.yml
flatpak run io.github.deimen24.PenguDeck
```

### From source

Requirements: a C11 compiler, Meson ≥ 0.62, GTK 4 ≥ 4.10, GLib ≥ 2.74,
json-glib, libcurl, FFmpeg ≥ 6.0 (libavformat, libavcodec, libavutil,
libswresample), Rubber Band ≥ 3 (optional, for keylock), and the
PulseAudio / ALSA / JACK client libraries you want to use at runtime
(they are loaded dynamically).

```sh
meson setup build
meson compile -C build
meson test -C build
./build/pengu-deck [file ...]
```

CI builds and packages every push for Arch, Ubuntu, Fedora and Flatpak,
see `.github/workflows/ci.yml`.

## SoundCloud

SoundCloud has no public API keys anymore, so Pengu Deck talks to the
same `api-v2` endpoints the web player uses:

1. Open **Preferences → SoundCloud** and press **Detect**. This reads the
   public client ID out of the soundcloud.com player scripts. The ID
   changes every few weeks; press Detect again when searches start
   failing.
2. Search, or paste a track / set / artist link into the SoundCloud tab
   and press Enter. Double click or drag a result onto a deck.
3. To load **your likes**, paste your OAuth token as well (log in on
   soundcloud.com, open the browser dev tools and copy the
   `oauth_token` cookie). It is stored in
   `~/.config/pengu-deck/settings.ini` with mode 0600.

Streams are fetched from SoundCloud's transcoding endpoints (progressive
MP3 preferred, HLS otherwise) and decoded progressively, so playback
starts within a second and the waveform fills in as data arrives.
Tracks limited to previews by their rights holders play as 30 second
snippets and are marked "(preview)". Encrypted streams cannot be played.

## Audio setup

- **Automatic** tries PulseAudio (which on PipeWire systems is
  `pipewire-pulse`), then ALSA, then JACK.
- **Buffer size** 256 frames is a good default; go to 128 or 64 on a
  PipeWire system with `pipewire-jack` for the lowest latency.
- **Headphone cue** needs either a 4 channel interface (mode "4 channel
  interface") or a splitter cable on a stereo output (mode "Split
  stereo": left ear = master, right ear = cue).
- Recordings land in `~/Music/Pengu Deck Mixes` as WAV files.

## Keyboard

| Deck A | Deck B | Action |
|--------|--------|--------|
| Q | P | Play / pause |
| W | O | Cue (hold to preview) |
| E | I | Sync to the other deck |
| A / S | K / L | Nudge back / forward |
| Z | M | 4 beat loop |
| X | , | Loop on / off |
| 1 2 3 4 | 7 8 9 0 | Hot cues |

← / → nudge the crossfader, Space plays deck A, Ctrl+L focuses the
library search, Ctrl+K opens the SoundCloud tab, Enter loads the selected
track into the free deck, Shift+Enter into deck B, Ctrl+scroll on a
waveform zooms.

## Files

- `~/.config/pengu-deck/settings.ini` – settings
- `~/.local/share/pengu-deck/tracks.ini` – cue points and analysed BPM
- `~/.cache/pengu-deck/library.tsv` – tag cache

## Layout of the source

```
src/track.c      progressively decoded PCM with lock free readers
src/decoder.c    FFmpeg decoding of files and streams, tag probing
src/analyze.c    onset envelope → tempo and beat grid
src/deck.c       playback, varispeed, keylock, loops, cues, EQ, filter
src/engine.c     audio device (miniaudio), mixer, headphone cue, recorder
src/soundcloud.c api-v2 client: search, resolve, likes, stream urls
src/library.c    folder scanning with a tag cache
src/ui/          GTK 4 widgets: decks, mixer, waveform, knobs, library
tests/           unit tests (tempo detection, DSP, decoder, parsing)
```

## License

GPL-3.0-or-later. miniaudio (third_party/) is public domain / MIT-0.
