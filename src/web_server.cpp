#include "web_server.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "display.h"
#include "sleep.h"
#include "storage.h"
#include "transcribe.h"
#include "wifi_manager.h"

// -----------------------------------------------------------------------
// SD file manager web UI (list / download / upload / delete on the SD
// root). Built on the ESP32 core's synchronous WebServer - no extra
// lib_deps needed. See web_server.h for the SPI-sharing constraint that
// shapes every handler below.
// -----------------------------------------------------------------------

static WebServer server(80);

// Root-only, no path traversal: reject anything with a slash, and dotfiles
// to match storage.cpp's catalog filter (macOS FAT litter). Empty or
// too-long names are rejected too - out is sized for an SD 8.3-or-longer
// filename plus the leading '/' handleDownload/handleDelete add.
static bool sanitize_name(const String &raw, char *out, size_t outLen) {
    if (raw.length() == 0 || raw.length() >= outLen - 1) {
        return false;
    }
    for (size_t i = 0; i < raw.length(); i++) {
        if (raw[i] == '/' || raw[i] == '\\') {
            return false;
        }
    }
    if (raw[0] == '.') {
        return false;
    }
    raw.toCharArray(out, outLen);
    return true;
}

// Brackets one request's SD access with display_suspend_touch()/sd_begin()
// (no-ops/real mount respectively on this board - see display.h). Pair
// every successful call with sd_release().
static bool sd_claim() {
    display_suspend_touch();
    if (!sd_begin()) {
        display_resume_touch();
        return false;
    }
    return true;
}

static void sd_release() {
    sd_end();
    display_resume_touch();
}

// Split around TRANSCRIBE_PROVIDER_JS below (handle_root() splices the
// three together) instead of one constant, so the browser-side Transcribe
// button's actual provider call - the one piece that has to match
// whichever transcribe_<provider>.cpp is compiled in - can be swapped by
// the same AI_PROVIDER_* build flag instead of a runtime branch. Split
// point is right before the main <script> block, so callProvider() (defined
// in TRANSCRIBE_PROVIDER_JS) is in place - hoisted by the time it's first
// called from an onclick handler - before transcribeFile() below uses it.
static const char INDEX_HTML_HEAD[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Annota - SD files</title>
<style>
  /* Visual language borrowed straight from the device's own e-paper screen
     (ui_epaper.cpp): a black status bar pinned across the top, bordered
     rounded "cards" for every row, and inversion (black bg / paper text)
     as the only hover/focus cue - in place of touch highlighting there,
     mouse hover here. Paper tone instead of pure white, sharp 1.5px black
     borders instead of shadows, no gradients or blur - mono first, color
     used only where it earns its keep (destructive actions). */
  :root {
    color-scheme: light;
    --paper: #eeece6;
    --surface: #fffffc;
    --ink: #14140f;
    --ink-soft: #5a594f;
    --border: #14140f;
    --danger: #a3271d;
    --danger-bg: rgba(163, 39, 29, 0.08);
  }
  * { box-sizing: border-box; }
  body {
    font-family: "Roboto", -apple-system, system-ui, sans-serif;
    background: var(--paper);
    color: var(--ink);
    max-width: 640px;
    margin: 0 auto;
    padding: 0 0 2rem;
  }
  .appbar {
    background: var(--ink);
    color: var(--surface);
    padding: 0.9rem 1rem;
    margin-bottom: 1rem;
  }
  .appbar h1 {
    margin: 0;
    font-size: 1rem;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }
  .appbar .sub { color: rgba(255, 255, 255, 0.6); font-size: 0.75rem; margin-top: 0.15rem; letter-spacing: 0.02em; }
  .appbar .row { display: flex; align-items: baseline; justify-content: space-between; }
  .appbar nav a {
    color: rgba(255, 255, 255, 0.6);
    text-decoration: none;
    font-size: 0.75rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    margin-left: 1rem;
    padding-bottom: 2px;
    border-bottom: 1px solid transparent;
  }
  .appbar nav a.active { color: var(--surface); border-bottom-color: var(--surface); }
  .card {
    background: var(--surface);
    border: 1.5px solid var(--border);
    border-radius: 6px;
    margin: 0 1rem 1.2rem;
    overflow: hidden;
  }
  #files-card { overflow-x: auto; }
  #player { padding: 0.9rem 1rem; }
  #player #playerName { font-size: 0.85rem; margin-bottom: 0.5rem; word-break: break-all; }
  #player #playerName::before { content: "\25B6  "; }
  #player audio { width: 100%; height: 32px; }
  #player #playerClose { margin-top: 0.4rem; color: var(--danger); border-color: var(--danger); }
  #player #playerClose:hover { background: var(--ink); color: var(--surface); border-color: var(--ink); }
  td.play { white-space: nowrap; }
  table { width: 100%; border-collapse: collapse; }
  th, td { text-align: left; padding: 0.65rem 0.8rem; vertical-align: middle; }
  thead th {
    font-size: 0.7rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--ink-soft);
    border-bottom: 1.5px solid var(--border);
  }
  tbody tr { border-bottom: 1px solid rgba(20, 20, 15, 0.12); }
  tbody tr:last-child { border-bottom: none; }
  tbody tr:hover { background: var(--ink); color: var(--surface); }
  tbody tr:hover td.date { color: rgba(255, 255, 255, 0.65); }
  th.sortable { cursor: pointer; user-select: none; white-space: nowrap; }
  th.sortable:hover { color: var(--ink); }
  .arrow { color: var(--ink); }
  td.size, th.size { text-align: right; white-space: nowrap; }
  td.date, th.date { white-space: nowrap; color: var(--ink-soft); font-size: 0.85rem; }
  td.actions { text-align: right; white-space: nowrap; }
  td.name { white-space: nowrap; }
  .file-icon { display: inline-block; width: 1.1em; text-align: center; opacity: 0.75; margin-right: 0.5rem; }
  table td:first-child, table th:first-child {
    max-width: 40vw;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  button, .btn {
    font: inherit;
    font-weight: 500;
    font-size: 0.78rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    cursor: pointer;
    border: 1.5px solid var(--border);
    background: transparent;
    color: var(--ink);
    padding: 0.4rem 0.6rem;
    border-radius: 4px;
    transition: background 0.12s ease, color 0.12s ease;
  }
  button:hover, .btn:hover { background: var(--ink); color: var(--surface); border-color: var(--ink); }
  button.danger { color: var(--danger); border-color: var(--danger); }
  button.danger:hover { background: var(--danger); color: var(--surface); border-color: var(--danger); }

  /* Table actions are icon-only (title attr carries the label) and a fixed
     square, so up to four in a row never wrap on narrow viewports. */
  td.actions button, td.actions .btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 1.8rem;
    height: 1.8rem;
    padding: 0;
    font-size: 0.85rem;
    text-decoration: none;
  }
  td.actions button + button, td.actions button + a, td.actions a + button { margin-left: 0.3rem; }
  tbody tr:hover td.actions button, tbody tr:hover td.actions .btn { border-color: var(--surface); color: var(--surface); }
  tbody tr:hover td.actions button:hover, tbody tr:hover td.actions .btn:hover { background: var(--surface); color: var(--ink); }
  tbody tr:hover button.danger, tbody tr:hover .btn.danger { color: var(--danger); border-color: var(--danger); }

  #drop {
    margin: 0 1rem 1.2rem;
    padding: 1.6rem 1rem;
    border: 1.5px dashed rgba(20, 20, 15, 0.3);
    border-radius: 6px;
    text-align: center;
    color: var(--ink-soft);
  }
  #drop.over { border-color: var(--ink); border-style: solid; background: rgba(20, 20, 15, 0.04); }
  #drop .btn { background: var(--surface); color: var(--ink); display: inline-block; margin-top: 0.4rem; }
  #drop .btn:hover { background: var(--ink); color: var(--surface); }
  #status { margin-top: 0.7rem; font-size: 0.85rem; color: var(--ink-soft); }
  progress {
    width: 100%;
    margin-top: 0.7rem;
    display: none;
    height: 6px;
    border-radius: 3px;
    overflow: hidden;
    border: 1px solid var(--border);
  }
  progress::-webkit-progress-bar { background: var(--surface); }
  progress::-webkit-progress-value { background: var(--ink); }
  progress::-moz-progress-bar { background: var(--ink); }
  #empty { color: var(--ink-soft); text-align: center; padding: 1.2rem; margin: 0 1rem 1.2rem; }
</style>
</head>
<body>
<div class="appbar">
  <div class="row">
    <h1>Annota</h1>
    <nav><a class="active" href="/">Files</a><a href="/settings">Settings</a></nav>
  </div>
  <div class="sub">SD card files</div>
</div>

<div id="player" class="card" hidden>
  <div id="playerName"></div>
  <audio id="playerAudio" controls></audio>
  <button id="playerClose" class="btn">✕ Stop</button>
</div>

<div class="card" id="files-card">
  <table id="files">
    <thead><tr>
      <th class="sortable" data-sort="name">Name<span class="arrow"></span></th>
      <th class="sortable date" data-sort="mtime">Date<span class="arrow"></span></th>
      <th class="size">Size</th>
      <th class="actions"></th>
    </tr></thead>
    <tbody></tbody>
  </table>
</div>
<div id="empty" class="card" hidden>No files on the card.</div>

<div id="drop">
  Drop a file here, or
  <label class="btn">choose one<input id="picker" type="file" style="display:none"></label>
  <progress id="progress" max="100" value="0"></progress>
  <div id="status"></div>
</div>
)rawliteral";

// The browser-side Transcribe button's provider call (callProvider(key,
// blob, filename), returning the transcript text or throwing) - the one
// piece of INDEX_HTML that has to match whichever transcribe_<provider>.cpp
// is compiled in, so it's picked by the same AI_PROVIDER_* flag instead of
// a runtime branch. Each variant mirrors its C++ counterpart's request
// shape (model, field names, endpoint) - see that file for why it's shaped
// the way it is. transcribe.cpp already #errors at compile time if no
// AI_PROVIDER_* is defined, so this only needs to handle the ones that
// exist.
#if defined(AI_PROVIDER_OPENAI)
static const char TRANSCRIBE_PROVIDER_JS[] PROGMEM = R"rawliteral(
<script>
// Mirrors transcribe_openai.cpp's request (whisper-1, multipart file
// upload) - talks to OpenAI directly from the browser instead of routing
// through the device.
async function callProvider(key, blob, filename) {
  const form = new FormData();
  form.append("model", "whisper-1");
  form.append("file", blob, filename);
  const res = await fetch("https://api.openai.com/v1/audio/transcriptions", {
    method: "POST",
    headers: { "Authorization": "Bearer " + key },
    body: form,
  });
  const json = await res.json();
  if (!res.ok) throw new Error((json.error && json.error.message) || ("HTTP " + res.status));
  return json.text;
}
</script>
)rawliteral";
#elif defined(AI_PROVIDER_GEMINI)
static const char TRANSCRIBE_PROVIDER_JS[] PROGMEM = R"rawliteral(
<script>
// Mirrors transcribe_gemini.cpp's request (generateContent, audio inlined
// as base64, same prompt) - talks to Gemini directly from the browser
// instead of routing through the device.
function blobToBase64(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result.split(",")[1]);
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(blob);
  });
}
async function callProvider(key, blob, filename) {
  const mimeType = /\.m4a$/i.test(filename) ? "audio/mp4" : "audio/mpeg";
  const data = await blobToBase64(blob);
  const body = {
    contents: [{
      parts: [
        { text: "Transcribe this audio recording verbatim. Respond with only the transcript text, no commentary." },
        { inline_data: { mime_type: mimeType, data } },
      ],
    }],
  };
  const url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.7-flash:generateContent?key=" +
    encodeURIComponent(key);
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const json = await res.json();
  const parts = json.candidates && json.candidates[0] && json.candidates[0].content && json.candidates[0].content.parts;
  const text = parts && parts[0] && parts[0].text;
  if (!res.ok || typeof text !== "string") throw new Error((json.error && json.error.message) || "Unexpected response from Gemini");
  return text;
}
</script>
)rawliteral";
#endif

static const char INDEX_HTML_TAIL[] PROGMEM = R"rawliteral(
<script>
function fmtSize(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / (1024 * 1024)).toFixed(1) + " MB";
}

function fmtDate(mtime) {
  if (!mtime) return "Unknown date";
  const d = new Date(mtime * 1000);
  return d.toLocaleString(undefined, { year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" });
}

let currentFiles = [];
let sortKey = "name";
let sortDir = 1; // 1 = ascending, -1 = descending

// Keep in sync with web_server.cpp's audio_content_type() - only this
// extension gets a Play button and a working /api/play.
function isAudio(name) {
  return /\.wav$/i.test(name);
}

function playFile(name) {
  const player = document.getElementById("player");
  const audio = document.getElementById("playerAudio");
  document.getElementById("playerName").textContent = name;
  audio.src = "/api/play?name=" + encodeURIComponent(name);
  player.hidden = false;
  player.scrollIntoView({ behavior: "smooth", block: "nearest" });
  audio.play();
}

document.getElementById("playerClose").onclick = () => {
  const audio = document.getElementById("playerAudio");
  audio.pause();
  audio.removeAttribute("src");
  audio.load();
  document.getElementById("player").hidden = true;
};

function render() {
  const sorted = currentFiles.slice().sort((a, b) => {
    let cmp;
    if (sortKey === "name") {
      cmp = a.name.localeCompare(b.name, undefined, { sensitivity: "base" });
    } else {
      cmp = a.mtime - b.mtime;
    }
    return cmp * sortDir;
  });

  document.querySelectorAll("th.sortable").forEach((th) => {
    const arrow = th.querySelector(".arrow");
    arrow.textContent = th.dataset.sort === sortKey ? (sortDir === 1 ? " ▲" : " ▼") : "";
  });

  const body = document.querySelector("#files tbody");
  body.innerHTML = "";
  document.getElementById("empty").hidden = sorted.length > 0;
  for (const f of sorted) {
    const tr = document.createElement("tr");

    const name = document.createElement("td");
    name.className = "name";
    const icon = document.createElement("span");
    icon.className = "file-icon";
    icon.textContent = isAudio(f.name) ? "♪" : "☰"; // matches ui_epaper.cpp's LV_SYMBOL_AUDIO / LV_SYMBOL_FILE distinction
    name.appendChild(icon);
    name.appendChild(document.createTextNode(f.name));
    tr.appendChild(name);

    const date = document.createElement("td");
    date.className = "date";
    date.textContent = fmtDate(f.mtime);
    tr.appendChild(date);

    const size = document.createElement("td");
    size.className = "size";
    size.textContent = fmtSize(f.size);
    tr.appendChild(size);

    const actions = document.createElement("td");
    actions.className = "actions";

    // Icon-only (see td.actions' CSS comment) - title carries the label
    // for a11y/tooltip instead of visible text, so up to four fit one row.
    if (isAudio(f.name)) {
      const play = document.createElement("button");
      play.textContent = "▶";
      play.title = "Play";
      play.onclick = () => playFile(f.name);
      actions.appendChild(play);

      const transcribe = document.createElement("button");
      transcribe.textContent = "✎";
      transcribe.title = "Transcribe";
      transcribe.onclick = () => transcribeFile(f.name, transcribe);
      actions.appendChild(transcribe);
    }

    const dl = document.createElement("a");
    dl.className = "btn";
    dl.href = "/api/download?name=" + encodeURIComponent(f.name);
    dl.textContent = "⬇";
    dl.title = "Download";
    actions.appendChild(dl);

    const del = document.createElement("button");
    del.className = "danger";
    del.textContent = "✕";
    del.title = "Delete";
    del.onclick = () => removeFile(f.name);
    actions.appendChild(del);

    tr.appendChild(actions);
    body.appendChild(tr);
  }
}

async function refresh() {
  const res = await fetch("/api/files");
  currentFiles = await res.json();
  render();
}

document.querySelectorAll("th.sortable").forEach((th) => {
  th.onclick = () => {
    if (sortKey === th.dataset.sort) {
      sortDir *= -1;
    } else {
      sortKey = th.dataset.sort;
      sortDir = 1;
    }
    render();
  };
});

// Runs the transcription entirely from the browser: the device only hands
// over the saved API key (GET /api/transcript-key) and the audio bytes
// (the existing /api/download), then stores whatever text comes back
// (POST /api/transcript) - the device itself never talks to the AI
// provider for this button. The actual provider call is callProvider(),
// defined just above in TRANSCRIBE_PROVIDER_JS - picked at build time by
// the same AI_PROVIDER_* flag that selects transcribe_<provider>.cpp on
// the device side, so this function itself has nothing provider-specific
// in it.
async function transcribeFile(name, btn) {
  const status = document.getElementById("status");
  const icon = btn.textContent; // restored in finally - button stays icon-only, see td.actions' CSS comment
  btn.disabled = true;
  try {
    const keyRes = await fetch("/api/transcript-key");
    const { key, providerName } = await keyRes.json();
    if (!key) {
      alert("No " + providerName + " API key set. Add one on the Settings page first.");
      return;
    }

    btn.textContent = "…";
    status.textContent = "Transcribing " + name + "...";
    const audioBlob = await (await fetch("/api/download?name=" + encodeURIComponent(name))).blob();

    const text = await callProvider(key, audioBlob, name);

    const saveRes = await fetch("/api/transcript?name=" + encodeURIComponent(name), {
      method: "POST",
      headers: { "Content-Type": "text/plain" },
      body: text,
    });
    if (!saveRes.ok) throw new Error("saving transcript failed: " + (await saveRes.text()));

    status.textContent = "Transcribed " + name;
    refresh();
  } catch (e) {
    status.textContent = "Transcribe failed: " + e.message;
  } finally {
    btn.disabled = false;
    btn.textContent = icon;
  }
}

async function removeFile(name) {
  if (!confirm("Delete " + name + "? This can't be undone.")) return;
  const res = await fetch("/api/delete?name=" + encodeURIComponent(name), { method: "POST" });
  if (!res.ok) {
    alert("Delete failed: " + (await res.text()));
  }
  refresh();
}

function uploadFile(file) {
  const status = document.getElementById("status");
  const progress = document.getElementById("progress");
  const form = new FormData();
  form.append("file", file, file.name);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/upload");
  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    progress.style.display = "block";
    progress.value = (e.loaded / e.total) * 100;
  };
  xhr.onload = () => {
    progress.style.display = "none";
    status.textContent = xhr.status === 200 ? "Uploaded " + file.name : "Upload failed: " + xhr.responseText;
    refresh();
  };
  xhr.onerror = () => {
    progress.style.display = "none";
    status.textContent = "Upload failed";
  };
  status.textContent = "Uploading " + file.name + "...";
  xhr.send(form);
}

const picker = document.getElementById("picker");
picker.onchange = () => { if (picker.files[0]) uploadFile(picker.files[0]); picker.value = ""; };

const drop = document.getElementById("drop");
drop.ondragover = (e) => { e.preventDefault(); drop.classList.add("over"); };
drop.ondragleave = () => drop.classList.remove("over");
drop.ondrop = (e) => {
  e.preventDefault();
  drop.classList.remove("over");
  if (e.dataTransfer.files[0]) uploadFile(e.dataTransfer.files[0]);
};

refresh();
</script>
</body>
</html>
)rawliteral";

// The device's only Settings UI (there's no on-screen equivalent on this
// board): WiFi/clock status, SD card info, Reconnect WiFi, and the Delete
// WiFi Setup danger button - same palette and card look as INDEX_HTML
// above.
static const char SETTINGS_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Annota - Settings</title>
<style>
  /* Same paper/ink language as INDEX_HTML - see its <style> comment. */
  :root {
    color-scheme: light;
    --paper: #eeece6;
    --surface: #fffffc;
    --ink: #14140f;
    --ink-soft: #5a594f;
    --border: #14140f;
    --danger: #a3271d;
  }
  * { box-sizing: border-box; }
  body {
    font-family: "Roboto", -apple-system, system-ui, sans-serif;
    background: var(--paper);
    color: var(--ink);
    max-width: 640px;
    margin: 0 auto;
    padding: 0 0 2rem;
  }
  .appbar {
    background: var(--ink);
    color: var(--surface);
    padding: 0.9rem 1rem;
    margin-bottom: 1rem;
  }
  .appbar h1 { margin: 0; font-size: 1rem; font-weight: 600; letter-spacing: 0.04em; text-transform: uppercase; }
  .appbar .sub { color: rgba(255, 255, 255, 0.6); font-size: 0.75rem; margin-top: 0.15rem; letter-spacing: 0.02em; }
  .appbar .row { display: flex; align-items: baseline; justify-content: space-between; }
  .appbar nav a {
    color: rgba(255, 255, 255, 0.6);
    text-decoration: none;
    font-size: 0.75rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    margin-left: 1rem;
    padding-bottom: 2px;
    border-bottom: 1px solid transparent;
  }
  .appbar nav a.active { color: var(--surface); border-bottom-color: var(--surface); }
  .card {
    background: var(--surface);
    border: 1.5px solid var(--border);
    border-radius: 6px;
    margin: 0 1rem 1.2rem;
    padding: 1rem;
  }
  .card h2 {
    margin: 0 0 0.7rem;
    font-size: 0.7rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--ink-soft);
  }
  .row-item {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 0.5rem 0;
    border-bottom: 1px solid rgba(20, 20, 15, 0.12);
  }
  .row-item:last-child { border-bottom: none; }
  .row-item .label { color: var(--ink-soft); font-size: 0.85rem; }
  .row-item .value { font-size: 0.9rem; text-align: right; }
  .value.ok::before { content: "● "; }
  .value.warn::before { content: "▲ "; color: var(--danger); }
  .value.ok, .value.warn { color: var(--ink); }
  .bar {
    height: 6px;
    border-radius: 3px;
    background: var(--surface);
    border: 1px solid var(--border);
    overflow: hidden;
    margin-top: 0.6rem;
  }
  .bar .fill { height: 100%; background: var(--ink); }

  button, .btn {
    font: inherit;
    font-weight: 500;
    font-size: 0.8rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    cursor: pointer;
    border: 1.5px solid var(--border);
    width: 100%;
    background: var(--surface);
    color: var(--ink);
    padding: 0.65rem 0.8rem;
    border-radius: 4px;
    transition: background 0.12s ease, color 0.12s ease;
  }
  button:hover { background: var(--ink); color: var(--surface); }
  button:disabled { opacity: 0.5; cursor: default; background: var(--surface); color: var(--ink); }
  button.accent { background: var(--ink); color: var(--surface); }
  button.accent:hover { background: var(--surface); color: var(--ink); }
  button.danger { color: var(--danger); border-color: var(--danger); background: var(--surface); }
  button.danger:hover { background: var(--danger); color: var(--surface); }
  #status { margin-top: 0.7rem; font-size: 0.85rem; color: var(--ink-soft); text-align: center; }
</style>
</head>
<body>
<div class="appbar">
  <div class="row">
    <h1>Annota</h1>
    <nav><a href="/">Files</a><a class="active" href="/settings">Settings</a></nav>
  </div>
  <div class="sub">Device settings</div>
</div>

<div class="card">
  <h2>Status</h2>
  <div class="row-item"><span class="label">WiFi</span><span class="value" id="wifiValue">-</span></div>
  <div class="row-item"><span class="label">Clock</span><span class="value" id="clockValue">-</span></div>
</div>

<div class="card">
  <h2>SD card</h2>
  <div class="row-item"><span class="label">Capacity</span><span class="value" id="cardValue">-</span></div>
  <div class="row-item"><span class="label">Space used</span><span class="value" id="usedValue">-</span></div>
  <div class="bar"><div class="fill" id="usedBar" style="width:0%"></div></div>
  <div class="row-item" style="margin-top:0.4rem"><span class="label">Audio files</span><span class="value" id="audioValue">-</span></div>
  <div class="row-item"><span class="label">Text files</span><span class="value" id="textValue">-</span></div>
</div>

<div class="card">
  <h2>WiFi</h2>
  <button class="accent" id="reconnectBtn">↻ Reconnect WiFi</button>
</div>

<div class="card">
  <h2 id="keyTitle">AI API Key</h2>
  <div class="row-item"><span class="label">Status</span><span class="value" id="keyValue">-</span></div>
  <input id="keyInput" type="password" placeholder="sk-... (leave blank to keep current)"
    style="width:100%;margin-top:0.6rem;padding:0.6rem 0.7rem;border-radius:4px;border:1.5px solid var(--border);background:var(--surface);color:var(--ink);font:inherit;box-sizing:border-box;">
  <button class="accent" id="keySaveBtn" style="margin-top:0.6rem;">Save API Key</button>
  <button class="danger" id="keyClearBtn" style="margin-top:0.6rem;">✕ Clear API Key</button>
</div>

<div class="card">
  <h2>Danger zone</h2>
  <button class="danger" id="forgetBtn">✕ Delete WiFi Setup</button>
</div>

<div id="status"></div>

<script>
function fmtGb(bytes) { return (bytes / 1000000000).toFixed(2) + " GB"; }

async function refresh() {
  let info;
  try {
    const res = await fetch("/api/settings");
    info = await res.json();
  } catch (e) {
    document.getElementById("status").textContent = "Lost connection to device";
    return;
  }

  const wifiValue = document.getElementById("wifiValue");
  if (info.wifiConnected) {
    wifiValue.textContent = info.ip;
    wifiValue.className = "value ok";
  } else {
    wifiValue.textContent = "working offline";
    wifiValue.className = "value warn";
  }
  document.getElementById("reconnectBtn").disabled = info.wifiConnected;

  const clockValue = document.getElementById("clockValue");
  clockValue.textContent = info.clockSynced ? "synced" : "not synced";
  clockValue.className = "value " + (info.clockSynced ? "ok" : "warn");

  document.getElementById("keyTitle").textContent = info.aiProviderName + " API Key";
  const keyValue = document.getElementById("keyValue");
  keyValue.textContent = info.aiKeyConfigured ? "set" : "not set";
  keyValue.className = "value " + (info.aiKeyConfigured ? "ok" : "warn");

  if (info.sdOk) {
    document.getElementById("cardValue").textContent = fmtGb(info.cardBytes);
    const usedPct = info.totalBytes > 0 ? (100 * info.usedBytes / info.totalBytes) : 0;
    document.getElementById("usedValue").textContent = usedPct.toFixed(1) + "%";
    document.getElementById("usedBar").style.width = usedPct.toFixed(1) + "%";
    document.getElementById("audioValue").textContent = info.audioFileCount;
    document.getElementById("textValue").textContent = info.textFileCount;
  } else {
    document.getElementById("cardValue").textContent = "unavailable";
    document.getElementById("usedValue").textContent = "-";
    document.getElementById("audioValue").textContent = "-";
    document.getElementById("textValue").textContent = "-";
  }
}

let pollTimer = null;

document.getElementById("reconnectBtn").onclick = async () => {
  const btn = document.getElementById("reconnectBtn");
  const status = document.getElementById("status");
  btn.disabled = true;
  status.textContent = "Reconnecting... this can take up to 30 seconds, and the page will briefly stop responding.";
  try {
    await fetch("/api/settings/reconnect", { method: "POST" });
  } catch (e) {
    // The device's web server is busy blocking on the reconnect attempt
    // itself (see wifi_manager.cpp) - that's expected, not a failure.
  }
  if (pollTimer) clearInterval(pollTimer);
  let tries = 0;
  pollTimer = setInterval(async () => {
    tries++;
    await refresh();
    if (tries > 40) {
      clearInterval(pollTimer);
      status.textContent = "";
    }
  }, 1000);
  setTimeout(() => { status.textContent = ""; }, 35000);
};

// The field never gets prefilled with the real saved key (see
// handle_settings_info()'s comment) - blank + Save is a no-op rather than
// an accidental clear; Clear is its own explicit, confirmed action.
document.getElementById("keySaveBtn").onclick = async () => {
  const input = document.getElementById("keyInput");
  const status = document.getElementById("status");
  if (!input.value) return;
  const form = new URLSearchParams();
  form.set("key", input.value);
  const res = await fetch("/api/settings/ai-key", { method: "POST", body: form });
  input.value = "";
  status.textContent = res.ok ? "API key saved." : "Save failed: " + (await res.text());
  refresh();
};

document.getElementById("keyClearBtn").onclick = async () => {
  if (!confirm("Clear the saved API key?")) return;
  const status = document.getElementById("status");
  const form = new URLSearchParams();
  form.set("key", "");
  const res = await fetch("/api/settings/ai-key", { method: "POST", body: form });
  status.textContent = res.ok ? "API key cleared." : "Clear failed: " + (await res.text());
  refresh();
};

document.getElementById("forgetBtn").onclick = async () => {
  if (!confirm("Delete the saved WiFi network and reboot into setup mode? This can't be undone from here - you'll need to join the device's setup WiFi network again.")) return;
  document.getElementById("status").textContent = "Rebooting into setup mode...";
  try {
    await fetch("/api/settings/forget", { method: "POST" });
  } catch (e) {
    // Expected: the device reboots mid-response.
  }
  document.getElementById("status").textContent = "Rebooted. Join the \"Annota-Setup\" WiFi network from your phone or laptop to reconfigure.";
};

refresh();
</script>
</body>
</html>
)rawliteral";

static void handle_root() {
    // Splices in TRANSCRIBE_PROVIDER_JS (picked by AI_PROVIDER_* above) -
    // small enough (a few KB total) that building it as one heap String per
    // request is simpler than a second send_P() and worth it to keep the
    // provider-specific piece out of the two main constants.
    String page = FPSTR(INDEX_HTML_HEAD);
    page += FPSTR(TRANSCRIBE_PROVIDER_JS);
    page += FPSTR(INDEX_HTML_TAIL);
    server.send(200, "text/html", page);
}

static void handle_settings_page() {
    server.send_P(200, "text/html", SETTINGS_HTML);
}

// GET /api/settings - snapshot for the settings page: live WiFi status
// (not cached - a plain WiFi.status() check), NTP sync state, and SD
// capacity/usage. SD access goes through the same
// display_suspend_touch()/display_resume_touch() bracket as everywhere
// else in this file (no-ops on this board, kept for symmetry) - unlike
// sd_claim()/sd_release(), get_sd_info() already calls sd_begin()/sd_end()
// itself, so only that bracket wraps it here.
static void handle_settings_info() {
    JsonDocument doc;
    bool connected = WiFi.status() == WL_CONNECTED;
    doc["wifiConnected"] = connected;
    doc["ip"] = connected ? WiFi.localIP().toString() : "";
    doc["clockSynced"] = wifi_clock_synced();
    // Never echoes the key itself here - the Settings page only learns
    // whether one's saved and shows a placeholder. The one place the raw
    // key does travel over the network is GET /api/transcript-key, for
    // the browser-side Transcribe button (see its handler's comment).
    // aiProviderName lets the page label the field correctly without
    // knowing which AI_PROVIDER_* is compiled in (transcribe.h).
    doc["aiProviderName"] = ai_provider_name();
    doc["aiKeyConfigured"] = ai_provider_has_api_key();

    display_suspend_touch();
    SdInfo info;
    bool sdOk = get_sd_info(info);
    display_resume_touch();

    doc["sdOk"] = sdOk;
    if (sdOk) {
        doc["cardBytes"] = info.cardBytes;
        doc["totalBytes"] = info.totalBytes;
        doc["usedBytes"] = info.usedBytes;
        doc["audioFileCount"] = info.audioFileCount;
        doc["textFileCount"] = info.textFileCount;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// POST /api/settings/reconnect - same request wifi_manager.h's
// wifi_request_reconnect() documents: only flags it, loop() actually
// runs it (wifi_process_pending_reconnect())
// once this handler has returned and lv_timer_handler() has run again, so
// this responds immediately rather than blocking the request for up to 30
// seconds. web_server_handle() itself won't run again until that attempt
// finishes, so the page's next few polls will stall rather than fail -
// the client-side handler above treats that as expected.
static void handle_settings_reconnect() {
    wifi_request_reconnect();
    server.send(200, "text/plain", "Reconnecting");
}

// POST /api/settings/forget - the "Delete WiFi Setup" button's handler;
// the confirmation happens client-side
// (confirm() in SETTINGS_HTML) since wifi_forget_and_reboot() itself does
// none and never returns. The response must be sent *before* calling it -
// once called, the device reboots and no code after it ever runs.
static void handle_settings_forget() {
    server.send(200, "text/plain", "Rebooting into setup mode");
    wifi_forget_and_reboot();
}

// POST /api/settings/ai-key - saves the API key field from SETTINGS_HTML,
// for whichever AI_PROVIDER_* is compiled in (transcribe.h). An empty
// `key` clears the saved one. Doesn't touch the SD card -
// ai_provider_set_api_key() is pure NVS (Preferences), so no
// sd_claim()/sd_release() dance is needed here.
static void handle_settings_set_ai_key() {
    if (!server.hasArg("key")) {
        server.send(400, "text/plain", "Missing key");
        return;
    }
    String key = server.arg("key");
    if (key.length() >= AI_API_KEY_MAX) {
        server.send(400, "text/plain", "Key too long");
        return;
    }
    ai_provider_set_api_key(key.c_str());
    server.send(200, "text/plain", "OK");
}

// GET /api/transcript-key - the one deliberate exception to
// handle_settings_info()'s "never echoes the key" rule: the browser-side
// Transcribe button (INDEX_HTML) calls the AI provider directly from the
// user's own browser rather than routing the upload through this device
// (see transcribe_openai.cpp's retry-loop comment for why offloading a
// multi-megabyte upload off the ESP32's flaky TLS stack is worth it), so
// it needs the raw key client-side. That's no new exposure in practice -
// this whole server is unauthenticated plain HTTP already (any other
// device on the LAN can already download/delete/upload files here), just
// the first endpoint that hands back a *secret* rather than a file.
static void handle_get_transcript_key() {
    char key[AI_API_KEY_MAX];
    ai_provider_get_api_key(key, sizeof(key));
    JsonDocument doc;
    doc["key"] = key;
    doc["providerName"] = ai_provider_name();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// POST /api/transcript?name=<audio file> - writes the raw POST body (the
// transcript text, sent as text/plain by the browser-side Transcribe
// button) to <name>'s sibling .txt file, overwriting any existing one.
// Mirrors transcribe_openai.cpp's txt_sibling_path()/write, since that's
// the on-device counterpart this replaces for files transcribed from the
// browser instead of from the on-screen UI.
static void handle_save_transcript() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }
    char name[64];
    if (!sanitize_name(server.arg("name"), name, sizeof(name))) {
        server.send(400, "text/plain", "Invalid name");
        return;
    }
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }
    String text = server.arg("plain");

    if (!sd_claim()) {
        server.send(503, "text/plain", "SD card not available");
        return;
    }

    const char *dot = strrchr(name, '.');
    size_t baseLen = dot ? (size_t)(dot - name) : strlen(name);
    char path[80];
    if (baseLen > sizeof(path) - 6) baseLen = sizeof(path) - 6; // '/' + baseLen + ".txt" + '\0'
    path[0] = '/';
    memcpy(path + 1, name, baseLen);
    strcpy(path + 1 + baseLen, ".txt");

    File f = sd_fs().open(path, FILE_WRITE);
    bool ok = (bool)f;
    if (ok) {
        f.print(text);
        f.close();
    }
    sd_release();

    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Write failed");
}

static void handle_list() {
    if (!sd_claim()) {
        server.send(503, "text/plain", "SD card not available");
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    File root = sd_fs().open("/");
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            const char *base = strrchr(entry.name(), '/');
            base = base ? base + 1 : entry.name();
            if (!entry.isDirectory() && base[0] != '.') {
                JsonObject o = arr.add<JsonObject>();
                o["name"] = base;
                o["size"] = entry.size();
                o["mtime"] = entry.getLastWrite(); // unix seconds, 0 if unknown - see storage.cpp's format_timestamp for the same fallback client-side
            }
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
    }
    sd_release();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// Returns the audio MIME type for a playable extension, or "" if `name`
// isn't one the <audio> element on the page offers a Play button for.
static String audio_content_type(const char *name) {
    size_t len = strlen(name);
    auto ends_with = [&](const char *ext) {
        size_t extLen = strlen(ext);
        return len > extLen && strcasecmp(name + len - extLen, ext) == 0;
    };
    if (ends_with(".wav")) return "audio/wav";
    return "";
}

// Streams a file inline (no Content-Disposition: attachment) so the page's
// <audio> element can play it directly instead of the browser downloading
// it. WebServer::streamFile() always sends "Accept-Ranges: none" (see
// WebServer.cpp), so there's no seek-ahead scrubbing - playback is
// sequential from the start, same as a plain <audio src>.
static void handle_play() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }
    char name[64];
    if (!sanitize_name(server.arg("name"), name, sizeof(name))) {
        server.send(400, "text/plain", "Invalid name");
        return;
    }
    String contentType = audio_content_type(name);
    if (contentType.length() == 0) {
        server.send(415, "text/plain", "Not a playable audio type");
        return;
    }

    if (!sd_claim()) {
        server.send(503, "text/plain", "SD card not available");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    if (!sd_fs().exists(path)) {
        sd_release();
        server.send(404, "text/plain", "Not found");
        return;
    }

    File f = sd_fs().open(path, FILE_READ);
    server.streamFile(f, contentType);
    f.close();
    sd_release();
}

static void handle_download() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }
    char name[64];
    if (!sanitize_name(server.arg("name"), name, sizeof(name))) {
        server.send(400, "text/plain", "Invalid name");
        return;
    }

    if (!sd_claim()) {
        server.send(503, "text/plain", "SD card not available");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    if (!sd_fs().exists(path)) {
        sd_release();
        server.send(404, "text/plain", "Not found");
        return;
    }

    File f = sd_fs().open(path, FILE_READ);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + String(name) + "\"");
    server.streamFile(f, "application/octet-stream");
    f.close();
    sd_release();
}

static void handle_delete() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }
    char name[64];
    if (!sanitize_name(server.arg("name"), name, sizeof(name))) {
        server.send(400, "text/plain", "Invalid name");
        return;
    }

    if (!sd_claim()) {
        server.send(503, "text/plain", "SD card not available");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    bool ok = sd_fs().remove(path);
    sd_release();

    if (ok) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(404, "text/plain", "Not found");
    }
}

// Shared between handle_upload_data() (fires per chunk, has no response
// channel) and handle_upload_done() (fires once the body is fully consumed,
// and is the only one of the two that can send a response).
static File uploadFile;
static bool uploadOk = false;
static bool uploadClaimed = false; // whether sd_release() is still owed

// Multipart upload body handler for POST /api/upload. Runs once per chunk
// across the whole request, so the SD claim spans UPLOAD_FILE_START through
// UPLOAD_FILE_END/ABORTED rather than one claim per call.
static void handle_upload_data() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadOk = false;
        char name[64];
        if (!sanitize_name(upload.filename, name, sizeof(name))) {
            return;
        }
        if (!sd_claim()) {
            return;
        }
        uploadClaimed = true;
        char path[80];
        snprintf(path, sizeof(path), "/%s", name);
        uploadFile = sd_fs().open(path, FILE_WRITE);
        uploadOk = (bool)uploadFile;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadOk) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadOk) {
            uploadFile.close();
        }
        if (uploadClaimed) {
            sd_release();
            uploadClaimed = false;
        }
        if (upload.status == UPLOAD_FILE_ABORTED) {
            uploadOk = false;
        }
    }
}

// Runs after handle_upload_data() has consumed the whole request body -
// sends the actual response, since the upload callback above can't.
static void handle_upload_done() {
    if (uploadOk) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Upload failed (bad filename, no SD card, or write error)");
    }
}

// Wraps a route handler so any served request also resets sleep.h's idle
// clock - without this, the idle-timeout deep sleep could fire mid-
// download/upload, or while someone's just sitting on the file manager
// page, purely because no onboard button was pressed in a while.
static WebServer::THandlerFunction with_activity(WebServer::THandlerFunction handler) {
    return [handler]() {
        sleep_reset_activity();
        handler();
    };
}

void web_server_start() {
    server.on("/", HTTP_GET, with_activity(handle_root));
    server.on("/settings", HTTP_GET, with_activity(handle_settings_page));
    server.on("/api/settings", HTTP_GET, with_activity(handle_settings_info));
    server.on("/api/settings/reconnect", HTTP_POST, with_activity(handle_settings_reconnect));
    server.on("/api/settings/forget", HTTP_POST, with_activity(handle_settings_forget));
    server.on("/api/settings/ai-key", HTTP_POST, with_activity(handle_settings_set_ai_key));
    server.on("/api/transcript-key", HTTP_GET, with_activity(handle_get_transcript_key));
    server.on("/api/transcript", HTTP_POST, with_activity(handle_save_transcript));
    server.on("/api/files", HTTP_GET, with_activity(handle_list));
    server.on("/api/download", HTTP_GET, with_activity(handle_download));
    server.on("/api/play", HTTP_GET, with_activity(handle_play));
    server.on("/api/delete", HTTP_POST, with_activity(handle_delete));
    server.on("/api/upload", HTTP_POST, with_activity(handle_upload_done), with_activity(handle_upload_data));
    server.onNotFound(with_activity([]() { server.send(404, "text/plain", "Not found"); }));
    server.begin();

    Serial.print("Web file manager: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
}

void web_server_handle() {
    server.handleClient();
}
