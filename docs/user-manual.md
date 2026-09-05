# Annota User Manual

Annota turns a Waveshare ESP32-S3-ePaper-1.54 board into a pocket voice
recorder and audio file browser: a 1.54" 200x200 black-and-white e-paper
screen, two buttons, an onboard microphone/speaker, and an SD card slot.
It records voice memos, plays back and organizes audio files on the SD
card, transcribes them to text using an AI provider (OpenAI by default),
and exposes a WiFi-based web file manager for the same SD card from any
browser on the same network.

This manual covers the on-device screen, the two-button controls, and the
web interface, as they exist in this version of the firmware.

## What you need

- The device, powered on (USB or its battery).
- A microSD card inserted, formatted FAT32, with audio files in the root
  folder (subfolders and hidden/dot files are ignored).
- Optionally, a WiFi network, to use the web file manager and AI
  transcription.

## The two buttons

The device has exactly two physical controls:

- **Next** (labeled BOOT on the board) — moves the selection or highlighted
  menu option forward.
- **Select** (labeled PWR on the board) — opens/confirms with a **short
  press**, backs out/cancels with a **long press** (about 0.7 seconds).

A few gestures build on top of that:

- **Long press Next** on the file list — switches between the Audio Files
  view and the Text Files (transcripts) view.
- **Long press Select** on the file list — opens the on-device Menu
  (Refresh / Offline↔Online / Reboot / Close).
- **Double-press Select quickly** (within about a third of a second) on any
  row but the first — instead of opening that file's action menu, jumps the
  selection straight back to the top "Record new" row. A single press still
  opens the action menu as usual; the device briefly waits to see if a
  second press is coming before acting.
- **Hold both buttons together for 5 seconds** — from anywhere, opens a
  confirmation to erase the saved WiFi network and reboot into first-time
  WiFi setup. This is the recovery gesture if the on-screen "Delete WiFi
  Setup" button in the web UI isn't reachable (e.g. no WiFi at all).

## Status bar

The black bar at the top of the screen is always visible, regardless of
which screen is showing below it:

- Left side: WiFi/status text (e.g. connection status, or a temporary
  message).
- Right side: battery percentage with a battery icon, and — if a card is
  inserted — an SD card icon.

Battery percentage is a voltage-based estimate (0–100%), not a precise fuel
gauge; expect it to dip during playback/recording and recover at rest.

## No SD card

If no SD card is detected, the screen shows "Insert an SD card to see your
audio files" and no navigation is available. Insert a card and reboot the
device (or use the on-device Menu's Refresh once the device recognizes it)
to load it.

## The file list

The main screen lists files from the SD card root, one of two modes at a
time:

- **Audio Files** — playable audio (mp3/wav and other configured audio
  extensions). This view also shows a **"Record new"** row pinned above the
  real files.
- **Text Files** — the `.txt` transcripts saved by transcription (see
  below). No "Record new" row here.

Long-press **Next** to switch between the two modes. The header row above
the list shows which mode you're in and how many files it holds; a small
down-arrow appears if there are more rows than fit on screen at once (the
list wraps around with **Next**, it doesn't stop at the last item).

- **Next** (short press) — move the highlighted row down one (wraps to the
  top).
- **Select** (short press) on a file — opens that file's **action menu**.
- **Select** (short press) on the "Record new" row — starts recording
  immediately (no action menu, since there's nothing else to choose there).
- **Select** (long press) — opens the on-device **Menu** (Refresh /
  Offline↔Online / Reboot / Close).

### Recording

Selecting "Record new" starts recording a new voice memo straight to the SD
card (plain 16-bit PCM WAV, no encoding). The screen shows "Recording
<filename>..."; press **Select** (short or long) to stop. The new file
appears in the Audio Files list immediately afterward. If the microphone
fails partway through, the screen shows the error message instead and
recording stops automatically.

Recording and playback share the same audio hardware and never run at the
same time; starting one stops the other.

### Action menu (per file)

Opened with a short **Select** press on a file row. **Next** cycles the
highlighted option, **Select** (short) chooses it, **Select** (long) backs
out to the list.

For audio files:
- **Play** — plays the file through the onboard speaker. Press **Select**
  to stop; the screen returns to the list when playback finishes on its
  own too.
- **Transcribe** — sends the file to the configured AI provider (see
  "Transcription" below) and saves the result as a `.txt` file with the
  same name.
- **Details** — shows the filename, creation date, and size. Press
  **Select** to close.
- **Delete** — asks to confirm, then deletes the file.
- **Cancel** — closes the menu.

For text files (transcripts), the menu only offers **Details**, **Delete**,
and **Cancel** — no Play/Transcribe, since there's nothing to play or
re-transcribe.

### On-device Menu

Opened with a long **Select** press from the file list:

- **Refresh** — re-scans the SD card and returns to the (current-mode)
  file list.
- **Offline** / **Online** — toggles WiFi. The label always reflects the
  live connection state. Choosing **Offline** disconnects and powers down
  the WiFi radio (saved network credentials are kept); choosing **Online**
  reconnects to the saved network. This does not erase any saved WiFi
  setup.
- **Reboot** — asks to confirm, then restarts the device.
- **Close** — closes the menu with no action.

## Transcription

Selecting **Transcribe** on an audio file uploads it to the configured AI
provider's speech-to-text API and saves the returned text as a sibling
`.txt` file (e.g. `memo1.wav` → `memo1.txt`). This version is built for
OpenAI's Whisper API by default (`whisper-1`); switching providers is a
compile-time choice.

- If the device is offline, it makes one attempt to reconnect to the saved
  WiFi network first. If that fails too, you'll see "No WiFi connection."
  instead of a transcript.
- An AI provider API key must already be set — from the web UI's Settings
  page (see below). Transcription fails with an error if no key is saved.
- The screen shows "Transcribing <filename>..." while the request is in
  flight (this blocks the device — no button input — since it's a network
  round-trip), then a result screen ("Transcription saved." or an error
  message). Press **Select** to close.
- The resulting `.txt` file shows up under the Text Files view (long-press
  **Next** from the list to switch there).

## WiFi setup

**First time / no network saved:** the device opens its own WiFi access
point named **"Annota-Setup"** (no password) and shows an on-screen prompt.
From a phone or laptop, join that network — a captive portal (or
`http://192.168.4.1`) lets you pick your home WiFi network and enter its
password. Once submitted, the device connects and remembers the network
for every future boot.

**Every boot after that:** the device reconnects to the saved network
automatically in the background while the rest of the UI comes up — no
setup portal, no blocking. If the router isn't reachable within about 10
seconds, the device just continues offline; the status bar reflects this,
but nothing modal interrupts whatever's on screen. Recording, playback,
deleting, and browsing all work fine offline — only transcription and the
web file manager need a network.

Every ~15 minutes, if the radio is on but the network has become
unreachable (not just a one-time reconnect failure), the device
automatically drops to offline mode on its own to save battery, rather
than burning power retrying against a network that's gone.

**Forgetting the saved network:** this is deliberately hard to trigger by
accident, since it's irreversible. Two ways:

1. Web UI → Settings → **Delete WiFi Setup** (with a confirmation).
2. On-device: hold both buttons together for 5 seconds, then confirm.

Either way, the saved credentials are erased and the device reboots
straight into the first-time setup portal.

A plain reconnect retry (without erasing anything) is available from the
on-device Menu's Offline↔Online toggle, or the web UI's **Reconnect WiFi**
button.

## Web file manager

Once the device is on WiFi, note the IP address shown in its status bar (or
the web UI's Settings page), and open `http://<that-ip>/` in a browser on
the same network. Two pages, reachable via the top nav bar:

### Files (`/`)

A table of every file on the SD card root: name, date, size, and per-row
actions. Features:

- **Upload** — drag a file onto the drop area, or use "choose one", to
  upload it to the SD card root. A progress bar tracks the upload.
- **Download** — per file, or in bulk via the selection checkboxes.
- **Play** — streams an audio file in-browser via a small player bar.
- **Transcribe** — per audio file (or in bulk, via the batch bar). This
  runs independently of the on-device Transcribe option: your browser
  fetches the saved API key from the device, downloads the audio file,
  sends it straight to the AI provider itself, and posts the resulting
  text back to the device to save — the device's own network stack is only
  briefly involved, not for the whole transcription. The result is the
  same sibling `.txt` file either way.
- **Delete** — per file, or in bulk, with confirmation.
- **Select all / batch actions** — check multiple rows to download,
  transcribe, or delete them together.
- Sortable columns (Name, Date).

Uploads and deletes made here update the SD card immediately, but the
on-device file list is only refreshed on the device's next reboot or its
own Menu → Refresh.

### Settings (`/settings`)

- **Status** — live WiFi connection state (IP address, or "working
  offline") and whether the system clock has synced over NTP.
- **SD card** — capacity, space used (with a usage bar), and a count of
  audio files vs. text files.
- **WiFi** — **Reconnect WiFi** button (disabled while already connected).
- **Power** — a slider (1–180 minutes) for how long the device stays idle
  before deep-sleeping to save battery. Takes effect immediately, no
  reboot needed.
- **<Provider> API Key** (e.g. "OpenAI API Key") — shows whether a key is
  currently saved (never the key's actual value, since this page is served
  over plain HTTP). Enter a new key and **Save API Key**, or **Clear API
  Key** to remove it. Leaving the field blank and saving does nothing.
- **Danger zone** — **Delete WiFi Setup**, with a confirmation prompt. See
  "Forgetting the saved network" above.

## Power and sleep

To save battery, the device automatically deep-sleeps after a period of
no activity (default 30 minutes; adjustable 1–180 minutes on the Settings
page, see above). "Activity" includes any button press and any request
served by the web file manager — so actively browsing/uploading/
downloading over WiFi, or a transcription in progress (on-device or via
the web UI), keeps it awake. Recording, playing back, or transcribing on
the device also blocks sleep outright while in progress.

Before sleeping, the screen shows "Sleeping... Hold Select to wake."

**To wake the device:** hold the **Select** button. (Next/BOOT does not
wake it — this is deliberate, so the on-screen instruction stays accurate.)
Waking is a full restart: the device boots up the same way it does from a
cold power-on, including re-scanning the SD card and reconnecting to WiFi.

## Troubleshooting

- **"Insert an SD card..."** — no card detected. Check it's seated
  correctly and formatted FAT32.
- **Device seems unresponsive after a long idle period** — it's likely
  asleep; hold Select to wake it.
- **Transcription fails with "No WiFi connection."** — the device is
  offline and couldn't reconnect to the saved network. Check the router,
  or use the on-device Menu's Online option / the web UI's Reconnect WiFi
  button.
- **Transcription fails despite being online** — check that an API key is
  saved on the Settings page.
- **Can't reach the web file manager** — confirm the device's WiFi status
  (status bar, or Settings page once reachable) and that your browser is
  on the same network. If the device is offline and you don't have the
  setup portal available, hold both buttons for 5 seconds to reset WiFi
  and start over.
- **Stuck in "Annota-Setup" mode after already configuring WiFi once** —
  this only happens after an explicit "Delete WiFi Setup" (or the 5-second
  button-hold gesture). Reconfigure a network the same way as first-time
  setup.
