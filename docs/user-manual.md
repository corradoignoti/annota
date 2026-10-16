# Annota User Manual

Annota turns a Waveshare ESP32-S3 e-paper board into a pocket voice
recorder: record voice notes on the device, play them back, and turn them
into text with AI transcription (OpenAI by default) — each transcript
saved next to its audio file with an AI-written title and abstract on top.
A web file manager reaches the same SD card from any browser on your
network, and the whole card can also be mounted on a computer as a USB
drive.

Two boards are supported, running the same app:

- **ESP32-S3-ePaper-1.54** — 1.54" 200×200 e-paper screen, two buttons.
- **ESP32-S3-ePaper-3.97** — 3.97" 800×480 e-paper screen used upright
  (portrait), a 3-way rotary switch (Up / Press / Down) plus a BOOT
  button.

Where the two differ, this manual says so. The device UI and the web
pages are available in English, Italian and French (see
[Settings](#settings)).

## What you need

- The device, powered on (USB or its battery).
- A microSD card, formatted FAT32. Audio files (`.mp3`, `.wav`) go in the
  root folder; subfolders and hidden/dot files are ignored.
- Optionally, a WiFi network and an AI provider API key, for
  transcription and the web file manager. Recording, playback, deleting,
  reading transcripts and USB drive mode all work offline.

## Controls

### 1.54 (two buttons)

- **Next** (labeled BOOT) — moves the highlight forward.
- **Select** (labeled PWR) — **short press** opens/confirms, **long
  press** (about 0.7 s) backs out or cancels.

### 3.97 (rotary switch + BOOT)

- **Down** — moves the highlight forward (same as Next on the 1.54).
- **Up** — moves the highlight back.
- **Press** — the Select button: short press opens/confirms, long press
  backs out.
- **BOOT** — goes back (same as a long Select). On the Notes list it opens
  the Home screen.

### On both boards

- The bottom line of every screen is a hint showing what the buttons do
  right now.
- **Hold both buttons for 5 seconds to reboot** — Next + Select on the
  1.54, Press + BOOT on the 3.97. Works even if the device seems frozen.

## Status bar

The black bar at the top is always visible:

- Left: WiFi status — the device's IP address when connected, blank when
  WiFi is off, or a short message while connecting.
- Right: battery percentage and, when a card is inserted, an SD card
  icon.

On the 1.54 the battery percentage is estimated from the battery voltage
and dips during playback/recording. The 3.97 reads it from its power
management chip; with no battery fitted it shows 100%.

## Notes list

The main screen lists every audio file on the card — your **notes**. A
**Record new** row is always at the top. Under each note's filename, the
AI title of its transcript is shown once it has been transcribed.

Notes not yet transcribed come first, then transcribed ones, newest
transcript first.

- **Next** / **Down** — move down (wraps around). **Up** (3.97) — move up.
- **Select** on **Record new** — starts recording straight away.
- **Select** on a note — opens its **action menu**.
- **Long press Select** — opens the device **Menu**.
- **Long press Next** (1.54) or **BOOT** (3.97) — opens the **Home**
  screen.
- Jump back to the top: **double-press Select** quickly on the 1.54;
  **hold Up** on the 3.97.

If no card is detected, the screen shows "Insert an SD card to see your
audio files" instead. Insert one and reboot.

### Recording

Recording writes a new `RECnnnn.wav` file (uncompressed 16-bit WAV) to the
card. The screen shows "Recording RECnnnn.wav..."; press **Select** to
stop. The new note appears at the top of the list. If the microphone or
the card fails, an error message replaces the recording screen.

Recording and playback share the audio hardware, so starting one stops
the other.

### Action menu

Opened with **Select** on a note:

- **Play** — plays the note through the speaker. **Select** stops; the
  list comes back by itself when the track ends.
- **Transcribe** — sends the note to the AI provider (see
  [Transcription](#transcription)). Once a note has a transcript, this
  item becomes **View transcription** instead.
- **View transcription** — opens the transcript for reading. On the 1.54,
  **Select** scrolls down, **Next** scrolls up, **long press Select**
  closes. On the 3.97, **Down**/**Up** scroll and **BOOT** closes.
- **Details** — name, size and playing time, plus the transcript's size
  and date when there is one. **Select** goes back.
- **Delete** — asks to confirm, then deletes the note. If the note has a
  transcript, the confirmation warns you and deletes both the audio and
  its `.txt`.
- **Cancel** — closes the menu.

### Volume (3.97 only)

While a note is playing, **hold Up or Down for 3 seconds** to open the
Volume menu: **Low**, **Medium** or **High**. **Up**/**Down** move,
**Press** applies the level immediately (playback keeps going, so you can
try them), **BOOT** closes. The level is remembered.

### Menu

Opened with a long **Select** press on the Notes list:

- **Refresh** — re-scans the SD card.
- **Offline** / **Online** — turns WiFi off, or connects to a saved
  network. The label shows the action available right now. Saved networks
  are never erased from here.
- **Reboot** — asks to confirm, then restarts the device.
- **Close** — closes the menu.

## Home screen

A carousel, one item at a time (three items on the 3.97, four on the 1.54):

- **Notes** — re-scans the card and shows the Notes list.
- **File transfer** — connects to WiFi and shows the web file manager's
  address as text and a QR code to scan with a phone. **Select** closes
  the screen and turns WiFi off again. If no saved network is in range,
  it opens [WiFi management](#on-device-wifi-management) instead.
- **USB drive** — exposes the whole SD card to a computer as a USB drive
  (see [USB drive mode](#usb-drive-mode)).
- **Usage** (1.54 only) — shows the same summary the 3.97 shows on its
  sleep screen: number of notes, how many are not yet transcribed, battery
  level, SD card space used/total, and firmware version. **Select** goes
  back to Home.

**Next**/**Down** (and **Up** on the 3.97) cycle, **Select** chooses,
long **Select** or **BOOT** goes back to the Notes list.

## Transcription

**Transcribe** uploads the note to the AI provider and saves the result as
a `.txt` file with the same name (`REC0007.wav` → `REC0007.txt`). With
OpenAI (the default build), the text file contains:

1. a short AI-written **title**,
2. an **abstract** of the note,
3. the full **transcript**,

in the language that was spoken. If the title/abstract step fails, the
plain transcript is still saved.

What happens when you choose Transcribe:

- WiFi is off by default, so the device connects to a saved network first
  and turns WiFi off again when done (unless you had already switched it
  on yourself).
- If no saved network is reachable, the device scans and lists your saved
  networks that are in range; pick one to join and the transcription
  carries on. Back out to cancel.
- The progress screen shows each step — *1/5 Connecting WiFi*, *2/5
  Uploading* (with a progress bar), *3/5 Waiting for transcription*, *4/5
  Writing title & abstract*, *5/5 Saving* — then "Transcription saved." or
  an error. **Select** closes it. The device can't be used while this runs.
- On failure, a diagnostic log is saved next to the note as
  `<name>_error.txt` and named on the result screen. It is deleted again by
  the next successful transcription of that note.

Notes larger than **2 MB on the 1.54** or **4 MB on the 3.97** are too big
for the device to upload itself; the screen tells you to use the web file
manager's **Transcribe** button instead, which has no such limit.

An API key must be saved on the web [Settings](#settings) page first.

## WiFi

### First setup

With no network saved, the device opens its own open access point,
**Annota-Setup**, and shows a prompt. Join it from a phone or laptop; a
captive portal (or `http://192.168.4.1`) lets you pick your WiFi network
and enter its password. To skip setup and work offline, **long press
Select** (1.54) or press **BOOT** (3.97).

### Saved networks and reconnecting

The device remembers **up to 5 networks**; manage them on the web
Settings page. It never connects on its own at boot — WiFi stays off to
save battery until something needs it: a transcription, **File
transfer**, or **Online** in the Menu.

When connecting, it first tries the network you joined most recently by
hand, then any other saved network in range, strongest signal first. If
none is reachable it simply stays offline; only the status bar changes.

If WiFi has been left on but the network disappears, the device turns
WiFi off by itself after about 15 minutes.

### On-device WiFi management

Shown by **File transfer** when no saved network is in range:

- **Join an access point** — scans and lists the saved networks that are
  in range, strongest first. Pick one to connect. Networks you haven't
  saved are not listed — add them on the Settings page.
- **Create an AP** — starts an open access point named **Annota-AP** and
  shows how to reach the web UI through it, with no router needed. On the
  3.97 two QR codes appear together: scan the first to join Annota-AP,
  then the second to open the web page. **Select** stops the access point.

Once connected, a screen shows the device's web address as text and a QR
code. **Select** closes it and turns WiFi off.

### Starting over

To get back to the **Annota-Setup** portal, remove every saved network on
the Settings page and reboot.

## USB drive mode

**Home → USB drive** turns the device into a USB drive: connect it to a
computer with a USB cable and the SD card shows up like a memory stick.
Copy notes off, or drop audio files on, then **eject the drive on the
computer** — the device restarts and re-scans the card by itself.

- WiFi is turned off while the drive is mounted.
- Pulling the cable also ends the mode (the device restarts).
- To leave without a computer: **long press Select** (1.54) or **BOOT**
  (3.97). Eject the drive on the computer first if it was mounted, or
  files being copied may be damaged.
- Copies are fairly slow, so prefer the web file manager or the card
  reader for large batches.

## Web file manager

When the device is online, open `http://<device-ip>/` in a browser on the
same network. The address is in the status bar and on the **File
transfer** screen. Three pages, linked from the top bar: **Files**,
**Settings**, **About**.

### Files

A card at the top shows SD card space used/free. Below it:

**Notes** — one row per audio file, with its transcript folded in: the
AI title under the filename, the date and the size. Per-row actions:

- **Play** — plays the note in the browser.
- **Transcribe** — transcribes it from your browser (see below). Becomes
  **View transcription** once a transcript exists.
- **Download** — downloads the audio and, if present, its transcript.
- **Delete** — deletes the audio and its transcript, after confirming.

Tick rows (or **Select all**) to **Download**, **Transcribe** or
**Delete** several at once. Click **Name** or **Date** to sort; clicking
again reverses, a third time goes back to the device's own order
(untranscribed first, then newest transcript).

**Sync** transcribes, one after the other, every audio file that has no
transcript yet. Files that failed before (that only have an
`_error.txt`) are retried.

**Other files** — everything else on the card (stray `.txt` files, error
logs, files copied over USB), with view, download and delete.

Uploading from the browser is not supported; copy files with USB drive
mode or a card reader.

**How browser transcription works:** your browser fetches the API key
from the device, downloads the audio, sends it to the AI provider itself,
then sends the finished text back to the device to save. Progress is shown
step by step at the top of the page (download, upload, waiting, title &
abstract, saving). Because the device only serves the file and stores the
result, there is no size limit, and long notes upload more reliably than
from the device. Keep the page open until it finishes.

Changes made here show up on the device's Notes list by themselves the
next time it is on the Notes list or Home screen.

### Settings

- **Language** — English, Italiano or Français, for the device screen and
  the web pages. Applies immediately.
- **Status** — WiFi connection and whether the clock has synced.
- **SD card** — capacity, space used, number of audio and text files.
- **WiFi Networks** — the saved networks (up to 5), each with **Edit**
  (enter a new password) and **Remove**, plus a form to **Add network**
  (leave the password blank for an open network). **Reconnect WiFi**
  retries the connection; the page stops responding for up to 30 seconds
  while it does.
- **Power** — **Sleep after idle**, 1 to 180 minutes (default 30).
  Applies immediately.
- **API Key** (e.g. "OpenAI API Key") — shows whether a key is saved,
  never the key itself. Paste a new key and **Save API Key**, or **Clear
  API Key**. Saving an empty field keeps the current key.

### About

What Annota is, the board it runs on, and credits for the open-source
software, fonts and icons it uses.

## Power and sleep

After a period with no activity (default 30 minutes, set on the Settings
page) the device goes into deep sleep to save battery. Button presses and
any use of the web pages count as activity. It never sleeps while
recording, playing, transcribing (on the device or from the browser) or in
USB drive mode.

The sleep screen shows the Annota logo and how to wake the device. On the
3.97 it also shows a summary: number of notes, how many are not yet
transcribed, battery level, SD card space used, and firmware version
(on the 1.54, see **Home → Usage**).

**To wake:** hold **Select** on the 1.54 (BOOT/Next does not wake it);
press the knob on the 3.97. Waking is a full restart, the same as turning
the device on.

## Troubleshooting

- **"Insert an SD card..."** — no card detected. Check it is seated and
  formatted FAT32, then reboot.
- **Screen frozen or device unresponsive** — hold both buttons for 5
  seconds to reboot (Next + Select on the 1.54, Press + BOOT on the 3.97).
  If it's showing the sleep screen, it's asleep: wake it as above.
- **"No ... API key set"** — save a key on the Settings page.
- **"File too large to transcribe on the device"** — use **Transcribe** in
  the web file manager instead.
- **Transcription failed** — open the `<name>_error.txt` file named on the
  result screen (web file manager → Other files) for the details. Long
  notes on a weak WiFi signal are the most common cause; try again closer
  to the router, or transcribe from the browser.
- **"Could not join the WiFi network."** — the network chosen from the
  list didn't accept the connection. Check its password on the Settings
  page.
- **Can't reach the web file manager** — WiFi is off by default: use
  **Home → File transfer**, or **Menu → Online**, then use the address on
  screen from a device on the same network. With no router around, use
  **Create an AP** on the WiFi management screen.
- **Notes copied in over USB don't show up** — make sure they are in the
  card's root folder and end in `.mp3` or `.wav`, then use **Menu →
  Refresh**.
