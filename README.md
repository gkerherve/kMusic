# kMusic

A beautiful "now playing" screen for Pebble watches, fed by an Android companion app.
Works with YouTube Music, Spotify or any player that shows a media notification.

- **Album art** on the watch (64 colours, or dithered black and white), and the
  background colour follows each cover.
- Song, artist, progress bar (a ring on round watches), elapsed / total time.
- **Up** previous · **Select** play / pause · **Down** next
- **Hold Up / Down** volume up / down
- **Hold Select** then **Up** like 👍, **Select** song info, **Down** dislike 👎
- Song info: album, year, genre, track number, composer, length, player, volume.

## Parts
- `watch/` — Pebble app (C). Build with `pebble build`.
- `android/` — companion app (Kotlin, no dependencies). Reads the phone's active media
  session through notification access and talks to the watch with the standard
  PebbleKit Android broadcasts. Built by GitHub Actions on every push; download
  `kMusic-companion` from the latest run's artifacts.

## Setup
1. Install the APK, open kMusic, tap **Allow notification access**.
2. Install the watch app and open it while music is playing.
