# Pengu Deck

DJ software for Linux with two to four decks. Plays anything FFmpeg can
decode from your music folders and streams tracks from SoundCloud
straight into a deck.

![Pengu Deck](docs/screenshot.png)

## Features

- 2–4 decks, switchable live without interrupting the music
- Three band coloured waveforms, scratchable zoomed view, track overview
- BPM and beat grid detection with grid editing and tap tempo
- Key detection (Camelot), key shift and keylock (Rubber Band)
- Sync, sync lock, quantize, beat jump, pitch range ±8/16/50/100 %
- Hot cues, beat loops, loop roll, slip, censor, reverse
- Four channel mixer: trim, 3 band EQ with kills, filter, crossfader
  assignment and curves, master limiter, auto gain
- Effects unit per channel: echo, reverb, flanger, phaser, bit crusher,
  trance gate, synced to the deck tempo
- Eight pad sampler
- Automix with transitions sized from the tracks (see below)
- Library with playlists, history, preview deck, M3U and set list
  export, search tokens `bpm:120-128`, `key:8A`, `genre:techno`
- Rekordbox XML import: cues, beat grid, playlists
- SoundCloud: in-app login, search, links, likes, on-disk cache,
  played marks
- Recording to WAV, FLAC, MP3 or Opus and Icecast / Shoutcast
  broadcasting at the same time
- Microphone with talkover ducking
- Headphone cue on a 4 channel interface or split stereo
- MIDI controllers with learn mode (Pioneer DDJ, CDJs in MIDI mode, any
  class compliant device)
- PulseAudio / PipeWire, ALSA and JACK output

Not included: stems separation, DVS, video, Ableton Link.

## Installing

Packages for every release are on the
[Releases](https://github.com/deimen24/Pengu_Deck/releases) page.

### Arch Linux

```sh
cd packaging/arch && makepkg -si
```

For lower latency install `pipewire-jack` and pick the JACK backend in
Preferences.

### Fedora

```sh
sudo dnf install gcc meson ninja-build gtk4-devel json-glib-devel \
    libcurl-devel rubberband-devel pulseaudio-libs-devel alsa-lib-devel \
    ffmpeg-free-devel
meson setup build && meson compile -C build && sudo meson install -C build
```

Or `rpmbuild -bb packaging/fedora/pengu-deck.spec`.

### Debian / Ubuntu

```sh
sudo apt install meson ninja-build libgtk-4-dev libjson-glib-dev \
    libcurl4-openssl-dev libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev librubberband-dev libpulse-dev libasound2-dev
meson setup build && meson compile -C build && sudo meson install -C build
```

Or `dpkg-buildpackage -us -uc -b`.

### Flatpak

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.deimen24.PenguDeck.yml
flatpak run io.github.deimen24.PenguDeck
```

### From source

C11, Meson ≥ 0.62, GTK 4 ≥ 4.10, GLib ≥ 2.74, json-glib, libcurl,
FFmpeg ≥ 6.0, optionally Rubber Band ≥ 3 (keylock) and alsa-lib
(MIDI). Audio backends are loaded at runtime.

```sh
meson setup build && meson compile -C build && meson test -C build
./build/pengu-deck [file ...]
```

## Layout

**Played** next to the filter box lists only the tracks played in this
session (the green rows); press it again for the whole list.

Drag the divider above the Library tabs to give the library more room.
The decks and the mixer shrink to fit: loop controls go first, then hot
cues and platter, then modes and pitch, then the zoomed waveform, until
only title, full track overview and transport remain. A short window
takes room from the library, never from the decks. Drag it back down and everything
returns. The split is remembered.

## Automix

Queue tracks from the Library or SoundCloud tab (**+ Queue**, context
menu, or drag rows onto the **Automix** tab (the list stays where you are) or into the queue; audio
files from the file manager drop there too; Ctrl or Shift click selects
several) and press **AUTOMIX**. The Automix tab shows which decks play
now and next, and every queued row carries key, tempo and the pitch it
would need to follow the playing deck (green within the pitch range,
red out of reach). Drag rows to reorder, Delete removes. The queue alternates between decks A and B: as soon as a
deck is free the next track is loaded into it, so it is decoded and
analysed long before it is due. It starts at its cue point, tempo
matched when the pitch range allows it, and the crossfader glides over.
Everything stays hands on during a transition; **Next ⏭** mixes right
away. A track you load into the free deck yourself is played next
instead of the queue, and a deck you already started is never restarted.

With **Smart order** on (default) the queue is not played top to bottom:
the track that best follows the current one is picked, by the pitch
needed to match the tempo (half and double time count) and then by the
distance on the Camelot wheel. Off, the queue plays in order.

With **Auto length** on (default) each transition is planned from the
two tracks, so every mix has its own length:

- matched tempos: a beat aligned blend of at least 32 beats
- a quiet outro stretches the blend to cover it, up to 30 s
- a quiet intro up to 32 s is blended over so the first drop lands as
  the fade ends; a longer one is skipped
- tempos too far apart to sync: a cut within 8 s

The length is clamped to 3–45 s and to what is left of the outgoing
track; the slider value is the minimum. Off, the slider value is used.

With **Sync tempo** on the two decks meet halfway: 8 s before the blend
the playing deck eases towards the middle tempo, the incoming deck
starts at the rest of the difference, and the beats are kept locked
while the crossfader moves. Afterwards the new deck glides back to its
own tempo over 30 s. Touching a pitch fader while it glides takes it
over.

Silence is never played: a blend ends where the outgoing track's music
ends, whatever silence follows, and the incoming track starts after
any leading silence. Every blend is planned to start on a 16 beat
phrase of the outgoing track and to run to its end; the incoming track
comes in on a bar of its own grid, or a phrase when one is near. While
it runs:

- the crossfader moves on an equal power curve, eased in and out
- the lows are swapped: the outgoing lows leave over the first 60 %,
  the incoming lows arrive over the last 60 %
- the incoming mids start a little under and rise early, the outgoing
  mids dip late, and a low pass sweep takes the outgoing track's edge
  off in the last stretch; the highs are left to the crossfader
- from the middle of the blend both decks drift together towards the
  incoming track's own tempo, and the new deck finishes that glide on
  its own afterwards

EQ, filter and crossfader curve return to where they were after the
blend, or right away when automix is switched off during one.

## MIDI controllers

Controllers are picked up through the ALSA sequencer, including when hot
plugged. **Preferences → MIDI → Learn**, then move the control. Buttons,
faders, knobs and relative jog wheels are supported. A controller's
sound card shows up under **Preferences → Audio** with the ALSA backend,
so outputs 1/2 can carry master and 3/4 headphones.

Pro DJ Link over Ethernet is proprietary and not supported.

## SoundCloud

Pengu Deck uses the official SoundCloud API, so it needs an app of your
own registered at
[developers.soundcloud.com](https://developers.soundcloud.com/):

1. Register an app and set its redirect URI to
   `http://127.0.0.1:38472/callback`.
2. Enter the app's client ID and client secret in **Preferences →
   SoundCloud**. Search and public links work from here on.
3. Press **Log in to SoundCloud** in the SoundCloud tab: your browser
   opens SoundCloud's authorization page, you sign in and allow the
   app, and SoundCloud sends the browser back to Pengu Deck. This is
   OAuth 2.1 with PKCE; your password never passes through the app.

Then search, or paste a track, set or artist link. Your likes load
with the **♥ Likes** button. **Playlists** lists your own and liked
playlists: **Open** shows a playlist's tracks (playlist name in the
Album column), **+ Automix** queues the whole playlist. Blocked tracks
are left out, and with **Full tracks** on preview only tracks too. Streaming needs the login; search and
links work with the app credentials alone. Tokens are refreshed
automatically and kept in `~/.config/pengu-deck/settings.ini`; the
credentials can also come from the environment as
`SOUNDCLOUD_CLIENT_ID` and `SOUNDCLOUD_CLIENT_SECRET`. Rate limits are
respected: a 429 is retried with exponential backoff, and a failed
token exchange is not repeated for a minute.

SoundCloud's API terms ask for attribution when streaming: the list
names the uploader, marks tracks as SoundCloud, and "Open on
SoundCloud" in the context menu links to the track page, "Copy
SoundCloud link" puts its address on the clipboard. Streams are cached to
`~/.cache/pengu-deck/soundcloud/` while playing; cached tracks show ⬇
and the cache can be cleared in Preferences. Tracks limited to previews
by their rights holders play as 30 s snippets and are marked
"(preview)". SoundCloud decides this per track and per app; "Full tracks"
next to the search box hides preview only tracks from searches, links and
likes.

## Recording, broadcasting, microphone

**REC** records the master to `~/Music/Pengu Deck Mixes`, **LIVE**
streams it to the Icecast or Shoutcast server from **Preferences →
Record & Stream**; both can run at once. **MIC** opens the microphone
chosen under **Preferences → Audio** with talkover ducking.

## Keyboard

| Deck A | Deck B | Action |
|--------|--------|--------|
| Q | P | Play / pause |
| W | O | Cue |
| E | I | Sync |
| A / S | K / L | Nudge |
| Z | M | 4 beat loop |
| X | , | Loop on / off |
| 1 2 3 4 | 7 8 9 0 | Hot cues |

← / → crossfader, Enter loads the selection into the free deck,
Shift+Enter into deck B, Ctrl+L library search, Ctrl+K SoundCloud,
Ctrl+M Automix, Ctrl+, preferences.

## Files

- `~/.config/pengu-deck/` – settings and MIDI mapping
- `~/.local/share/pengu-deck/` – cues, beat grids, playlists, history
- `~/.cache/pengu-deck/` – tag cache and SoundCloud streams

## License

GPL-3.0-or-later. miniaudio (third_party/) is public domain / MIT-0.
