# TrimUI Brick Vibe

Build (needs Docker + internet): `./build.sh`
Install: copy `dist/Vibe` to `/mnt/SDCARD/Apps/` (Stock OS and CrossMix).
Music goes in `/mnt/SDCARD/Music` (scanned recursively). First launch builds a metadata cache (slow on big libraries); later launches are fast.

Tabs: L1/R1 switch Albums / Artists / Songs / Playlists / Download. A opens (artist→albums→tracks) or plays, B back, START shuffle the whole library, X shuffle on/off, Y now playing, SELECT screen off, MENU options.
Now playing: A pause, L1/R1 prev/next, Left/Right seek 10s, X shuffle, Y repeat, B back.
MENU: Resume / [Equalizer, Edit tags, Add to playlist on Now Playing] / Keep playing in background / Search songs / USB Mode / Settings (Theme, LED sync, Rescan songs) / Quit. SELECT turns the screen off.

Background control: `./ctl.sh toggle|play|pause|next|prev|show|quit` (writes /tmp/vibe.ctl).
Launching the app again while it is in the background brings the UI back.

Input debugging: create an empty file named `debug` in the app folder, press buttons, read `input.log`, then edit `buttons.ini`.

USB Music mode: MENU > "USB Music mode" shares only /Music with a PC over MTP (stock MtpDaemon in a private mount namespace). Test by hand: sh usbmusic.sh start / stop.

Background hotkeys (while the player is in background, e.g. during a game):
Menu+Start pause/play, Menu+R1 next, Menu+L1 previous. Configure in buttons.ini (hk_pause, hk_next, hk_prev; add @ms for long press).

Grid: 3x2, selection is raised with a thick glow. Now playing: L2/R2 cycle the visualizer (Off / Bars / Wave / Circle).
MENU > Theme: Default (glow follows album art) or a static color (Monet / VTuber presets). Edit themes.ini to tweak or add colors.
Themes: edit themes.ini (Name=GLOW,BACKGROUND,BARS in hex). Default theme: black background, glow follows album art.

Fonts: fonts/font.otf is Noto Sans CJK (Japanese, Korean, Chinese, Latin, Cyrillic, Greek). Any fonts/fb_* file is used as a per-character fallback (Arabic, Hebrew, Thai, Devanagari are downloaded by the build).
Screen off: only SELECT wakes the screen.

Menu: Rescan songs (stops playback and reloads /Music), wrap-around navigation, USB Mode (Linux Only).
Top bar shows clock and battery on the browse screens. buttons.ini: clock_12h, screen_ioctl, screen_blank, screen_off_cmd, screen_on_cmd.

Downloader: MENU > Download music (YouTube Music). Search with the on-screen keyboard, paste/type a URL or video id, or import Music/Downloads/links.txt.
Saved as 320 kbps MP3 with tags and square cover art in Music/Downloads. Tools are in bin/ (yt-dlp, qjs, ffmpeg); use "Update downloader" when YouTube breaks it.
After downloading, use MENU > Rescan songs.

Downloader details: songs are fetched in batches by one yt-dlp process (bin/yt-dlp), converted to 320 kbps MP3 by bin/ffmpeg, and tagged with title/artist/album and a square cover.
Failures are explained (network / unavailable / needs update). Downloads wait if the SD card has under 200 MB free.

Download tab > Search YouTube Music: keyboard on the left, results on the right as a list (song name, channel, length). Press START to search.

LEDs: MENU > LED sync. The top strip, F1/F2 keys and shoulder lights follow the current glow color while Vibe is on screen; stock LED settings are restored when you leave.

Now playing: START opens the current song's album.

Troubleshooting downloads: Vibe/dl.log records what the downloader saw (raw search rows). Send it along when reporting a problem.

Resume: Vibe remembers the song, position, shuffle/repeat and queue (resume.cfg). After a restart press Y on a browse screen, before playing anything, to continue where you left off.

Now Playing > MENU: Equalizer (10 bands, presets, X resets to flat; applies to all music) and Edit tags (title, artist, album, album artist, track, year, genre; rewrites the song's file).
The top bar always shows Wi-Fi and Bluetooth icons: white when on, grey when off.

Search uses YouTube Music; if it finds nothing it falls back to a plain YouTube search. YouTube Music results often have no channel name, so the second line can be blank.
Screen off: brightness is set to 0 through /dev/disp and restored on wake (like the stock OS). screen_backlight=1 / screen_blank=1 in buttons.ini are optional extras.

Playlists: the Playlists tab lists your playlists; the first row, "+ New Playlist", is always there (name it with the on-screen keyboard). A opens a playlist, A on a song plays from there, START shuffles it, X removes a song. In the playlist list, X (press twice) deletes a playlist. Add the current song from Now Playing > MENU > Add to playlist. Stored in Vibe/playlists.cfg (song paths, so renaming/moving files removes them from a playlist).
Keyboard: L2 (or R2) exits any keyboard. L1/R1 move the typing cursor left/right; letters are inserted and deleted at the cursor.
Now Playing tint for non-default themes: a darkened copy of the current glow color at TINT_ALPHA in src/main.c (204 = 80%).

Themes: Monet colors, then VTuber themes (Detective, Jerboa, Shark, Fish, Priestess, Phoenix, Death, Hope, Warden, Rat, Nature, Tapir, Owl, Orca, Space, Archiver, Demon, Rock, Hellhounds, Queen, Snow Leopard, Automaton, Gremlin).
A theme in themes.ini can have a 4th color (Name=GLOW,BACKGROUND,BARS,GLOW2): the selection glow, the grid glow and the progress bar then fade from GLOW (left) to GLOW2 (right). Hellhounds uses this (pink to light blue). The visualizer, LEDs and tab underline use the first color only.
Old theme names (Gawr Gura, Nanashi Mumei, Shiori Novella, Mori Calliope) are mapped to Shark, Owl, Archiver and Death if they appear in an old themes.ini or settings.cfg.

themes.ini: when the file has any themes, the Theme menu shows exactly Default + the themes in the file (the built-in colors are used only if there is no themes.ini).
