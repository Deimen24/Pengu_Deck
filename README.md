# Pengu Deck

Two deck DJ software for Linux. Plays anything FFmpeg can decode from your
music folders and streams tracks straight from SoundCloud into a deck.

![Pengu Deck](docs/screenshot.png)

## How it compares

Feature set checked against rekordbox, Serato DJ Pro, Traktor Pro 4,
VirtualDJ, djay Pro, Engine DJ and Mixxx (September 2026):

| Feature | Pengu Deck | Notes |
|---|---|---|
| 2–4 decks, hot cues, beat loops, loop roll, slip, censor, reverse | ✓ | switch deck count live |
| BPM / beat grid detection, grid editing, tap tempo | ✓ | |
| Key detection (Camelot), key shift, keylock | ✓ | Rubber Band |
| Sync lock, beat jump, quantize | ✓ | |
| Effects unit per channel | ✓ | echo, reverb, flanger, phaser, crush, gate |
| Sampler pads | ✓ | 8 pads |
| Automix | ✓ | queue, tempo sync, adjustable transition |
| Auto gain / replay gain | ✓ | |
| Library: playlists, history, smart search, preview deck | ✓ | bpm:, key:, genre: tokens |
| Rekordbox XML import (cues, grid, playlists) | ✓ | |
| Recording WAV / FLAC / MP3 / Opus | ✓ | |
| Icecast / Shoutcast broadcasting | ✓ | |
| Microphone with talkover | ✓ | |
| MIDI controllers with learn | ✓ | Pioneer DDJ, CDJ MIDI mode, any class compliant device |
| SoundCloud streaming with in-app login and cache | ✓ | Serato / djay / VirtualDJ have paid streaming tiers |
| Stems separation | ✗ | needs an ML model, planned |
| DVS timecode vinyl | ✗ | planned |
| Video mixing, karaoke, lighting | ✗ | out of scope |
| Ableton Link | ✗ | |

## Features

- Two, three or four decks, switchable at any time without interrupting
  the music (all four always run in the engine, the switch only changes
  what is shown)
- Three band coloured waveforms (bass / mids / highs), a zoomed
  scrolling view you can scratch with the mouse and a track overview
  for seeking
- Automatic BPM and beat grid detection, stored per track, plus tag BPM
- Sync (tempo and beat phase), pitch fader with ±8/16/50 % range, nudge
- Keylock: change tempo without changing key (Rubber Band)
- CDJ style cue, four hot cues and beat-quantised loops (¼ to 16 beats),
  manual in/out loops, loop halving and doubling
- Four channel mixer with trim, three band EQ with kill switches,
  low/high pass filter, channel faders, crossfader assignment per
  channel (A side / thru / B side) with three curves, master limiter
- Automix: queue tracks and let the app play them hands free on decks A
  and B, pre-loading the next track, starting it at its cue point,
  tempo synced when possible, and gliding the crossfader over an
  adjustable transition
- MIDI controllers with a learn mode (Pioneer DDJ, CDJs in MIDI mode
  and any class compliant device, hot plugged, no driver needed)
- Per channel effects unit (echo, reverb, flanger, phaser, bit crusher,
  trance gate) synced to the deck tempo, with wet and parameter knobs
- Eight pad sampler (one shot, loop, hold)
- Beat jump, slip mode, loop rolls, censor, reverse, quantize, sync
  lock, key shift and key detection with Camelot codes
- Beat grid editing: tap tempo, set downbeat, nudge, halve / double,
  re-analyse
- Auto gain to -18 dBFS RMS
- Library sidebar with playlists (crates) and play history, M3U and
  set list export, headphone pre-listen on every row, search tokens
  `bpm:120-128`, `key:8A`, `genre:techno`
- Rekordbox XML import: cues, beat grid, tags and playlists
- Recording to WAV, FLAC, MP3 or Opus and Icecast / Shoutcast
  broadcasting at the same time
- Microphone input with talkover ducking
- Headphone cueing: split stereo output (left = master, right = cue) or a
  4 channel audio interface (3/4 = cue), with cue/master mix
- Local library with tag reading, caching, search and sorting
- SoundCloud: search, paste track / playlist / artist links, load your
  likes; tracks you load or queue are cached on disk in the background
  and play from the cache from then on
- Played tracks are ticked (✓) until you reset the marks or close the
  app
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
    ffmpeg-free-devel
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
libswresample), Rubber Band ≥ 3 (optional, for keylock), alsa-lib
(optional, for MIDI), and the PulseAudio / ALSA / JACK client libraries
you want to use at runtime (they are loaded dynamically).

```sh
meson setup build
meson compile -C build
meson test -C build
./build/pengu-deck [file ...]
```

CI builds and packages every push for Arch, Ubuntu, Fedora and Flatpak,
see `.github/workflows/ci.yml`.

## Automix

Open the **Automix** tab, add tracks with the **+ Queue** button in the
Library or SoundCloud tab (or right click → "Add to automix queue", or
drag them onto the panel) and press **AUTOMIX**. The queue plays
alternately on decks A and B: the next track is loaded early, started
at its cue point when the transition begins (tempo matched if the
pitch range allows it) and the crossfader glides over. You can still
touch every knob and fader during a transition; **Next ⏭** mixes into
the next track right away. Queued SoundCloud tracks are downloaded as
soon as they are queued so the transition never waits for the network.

With **Auto length** ticked (the default) every transition is sized
from the two tracks instead of the fixed slider value:

- tempos that can be matched get a beat aligned blend of at least
  16 beats (7.5 s at 128 BPM, longer at slower tempos);
- a quiet outro on the ending track stretches the blend to cover it,
  up to 30 s, so a long fade out is never played over silence;
- tracks whose tempos are too far apart to sync are cut within 8 s;
- a long quiet intro on the incoming track is skipped when it has no
  cue point, so the new track comes in where it has energy.

The result is clamped to 3–45 s and to what is left of the outgoing
track; the status line shows the planned length ("15 s blend") as soon
as the next track is loaded.

## MIDI controllers

Pioneer DDJ controllers, CDJs switched to MIDI control mode and any
other class compliant controller are picked up automatically through the
ALSA sequencer (also with PipeWire), including devices plugged in while
the app runs. Open **Preferences → MIDI**, press **Learn** next to a
function and move the control on your device. Buttons, faders, knobs and
jog wheels (relative / two's complement encoders) are supported; the
mapping lives in `~/.config/pengu-deck/midi.ini`. A controller's own
sound card appears under **Preferences → Audio** with the ALSA backend,
so its 4 outputs can carry master (1/2) and headphones (3/4).

Pro DJ Link over Ethernet is Pioneer's proprietary player network and
is not supported.

## SoundCloud

SoundCloud has no public API keys anymore, so Pengu Deck talks to the
same `api-v2` endpoints the web player uses. The public client ID is
detected automatically at start.

1. Press **Log in to SoundCloud** in the SoundCloud tab and sign in in
   the embedded browser window (WebKitGTK). Your session token is picked
   up automatically and your likes load right away. **Log out** clears
   the session again. Without WebKitGTK, paste an OAuth token in
   **Preferences → SoundCloud** instead.
2. Search, or paste a track / set / artist link into the SoundCloud tab
   and press Enter. Double click or drag a result onto a deck, or press
   the headphone button to pre-listen.

Streams are fetched from SoundCloud's transcoding endpoints (progressive
MP3 preferred, HLS otherwise) and decoded progressively, so playback
starts within a second and the waveform fills in as data arrives. At the
same time the stream is remuxed into
`~/.cache/pengu-deck/soundcloud/<id>.mka`; the next load plays from
disk. The list shows ⬇ for cached tracks and a percentage while
downloading; "Download to cache" in the context menu fetches a track
ahead of time and **Preferences → SoundCloud** shows the cache size and
clears it.
Tracks limited to previews by their rights holders play as 30 second
snippets and are marked "(preview)". Encrypted streams cannot be played.

## Recording, broadcasting and microphone

**REC** in the mixer records the master to
`~/Music/Pengu Deck Mixes` in the format chosen in **Preferences →
Record & Stream** (WAV, FLAC, MP3 or Opus, encoders from FFmpeg).
**LIVE** streams the master to the Icecast or Shoutcast server set up
on the same page (MP3 or Opus); both can run at once. Enable the
microphone in **Preferences → Audio**, then **MIC** in the mixer opens
it with talkover: the music ducks by the configured amount while you
speak.

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
library search, Ctrl+K opens the SoundCloud tab, Ctrl+M the Automix
tab, Ctrl+, the preferences, Enter loads the selected track into the
free deck, Shift+Enter into deck B, Ctrl+scroll on a waveform zooms.
Decks C and D are controlled with the mouse or MIDI.

## Files

- `~/.config/pengu-deck/settings.ini` – settings
- `~/.config/pengu-deck/midi.ini` – MIDI mapping
- `~/.local/share/pengu-deck/tracks.ini` – cue points and analysed BPM
- `~/.cache/pengu-deck/library.tsv` – tag cache
- `~/.cache/pengu-deck/soundcloud/` – cached SoundCloud streams

## Layout of the source

```
src/track.c      progressively decoded PCM with lock free readers
src/decoder.c    FFmpeg decoding of files and streams, tag probing
src/analyze.c    onset envelope → tempo and beat grid
src/deck.c       playback, varispeed, keylock, loops, cues, EQ, filter
src/engine.c     audio device (miniaudio), mixer, headphone cue, recorder
src/soundcloud.c api-v2 client: search, resolve, likes, stream urls
src/sccache.c    background download of SoundCloud streams
src/automix.c    hands free playback of the queue
src/midi.c       ALSA sequencer input with a learnable mapping
src/library.c    folder scanning with a tag cache
src/ui/          GTK 4 widgets: decks, mixer, waveform, knobs, library
tests/           unit tests (tempo detection, DSP, decoder, parsing)
```

## License

GPL-3.0-or-later. miniaudio (third_party/) is public domain / MIT-0.
