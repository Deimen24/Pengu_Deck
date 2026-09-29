Name:           pengu-deck
Version:        0.1.0
Release:        1%{?dist}
Summary:        Two deck DJ software with local files and SoundCloud streaming
License:        GPL-3.0-or-later
URL:            https://github.com/deimen24/Pengu_Deck
Source0:        %{url}/archive/v%{version}/Pengu_Deck-%{version}.tar.gz

BuildRequires:  gcc meson ninja-build pkgconfig
BuildRequires:  pkgconfig(gtk4) >= 4.10
BuildRequires:  pkgconfig(glib-2.0) pkgconfig(gio-2.0) pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libavformat) pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavutil) pkgconfig(libswresample)
BuildRequires:  pkgconfig(rubberband)
BuildRequires:  pkgconfig(libpulse) pkgconfig(alsa)
BuildRequires:  desktop-file-utils
# FFmpeg with all codecs comes from RPM Fusion; ffmpeg-free works for
# MP3, FLAC, Vorbis, Opus and AAC decoding.
Requires:       hicolor-icon-theme

%description
Pengu Deck is a lightweight DJ application for Linux with two decks,
waveforms, BPM detection, sync, loops, hot cues, a three band EQ mixer,
keylock, headphone cueing, recording and SoundCloud streaming.

%prep
%autosetup -n Pengu_Deck-%{version}

%build
%meson -Dkeylock=enabled
%meson_build

%check
%meson_test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.deimen24.PenguDeck.desktop

%install
%meson_install

%files
%license LICENSE
%doc README.md
%{_bindir}/pengu-deck
%{_datadir}/applications/io.github.deimen24.PenguDeck.desktop
%{_datadir}/metainfo/io.github.deimen24.PenguDeck.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.deimen24.PenguDeck.svg

%changelog
* Tue Sep 29 2026 deimen24 - 0.1.0-1
- First release
