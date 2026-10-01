#include "web_server.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "display.h"
#include "i18n.h"
#include "sleep.h"
#include "storage.h"
#include "transcribe.h"
#include "ui.h"
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
<title data-i18n="title_files"></title>
<script src="/i18n.js"></script>
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
  /* #batchBar/#syncBar/#sd-widget set display: flex, which would
     otherwise beat the hidden attribute's UA display: none. */
  [hidden] { display: none !important; }
  body {
    font-family: "Roboto", -apple-system, system-ui, sans-serif;
    background: var(--paper);
    color: var(--ink);
    max-width: 760px;
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
  #files-card, #others-card { overflow-x: auto; }
  .card-title {
    padding: 0.65rem 0.8rem;
    font-size: 0.78rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    border-bottom: 1.5px solid var(--border);
  }
  /* A note's AI title (or "no transcript yet"), under its filename - the
     web counterpart of the device list's second line. */
  .note-title {
    white-space: normal;
    overflow-wrap: anywhere;
    font-size: 0.78rem;
    color: var(--ink-soft);
    margin: 0.15rem 0 0 1.6em;
  }
  .note-title.none { font-style: italic; }
  tbody tr:hover .note-title { color: rgba(255, 255, 255, 0.65); }
  #player { padding: 0.9rem 1rem; }
  #player #playerName { font-size: 0.85rem; margin-bottom: 0.5rem; word-break: break-all; }
  #player #playerName::before { content: "\25B6  "; }
  #player audio { width: 100%; height: 32px; }
  #player #playerClose { margin-top: 0.4rem; color: var(--danger); border-color: var(--danger); }
  #player #playerClose:hover { background: var(--ink); color: var(--surface); border-color: var(--ink); }

  #textViewer { padding: 0.9rem 1rem; }
  #textViewer #textViewerName { font-size: 0.85rem; margin-bottom: 0.5rem; word-break: break-all; }
  #textViewer #textViewerName::before { content: "\2630  "; }
  #textViewer #textViewerBody {
    max-height: 50vh;
    overflow-y: auto;
    white-space: pre-wrap;
    word-break: break-word;
    font-size: 0.85rem;
    padding: 0.7rem;
    border: 1.5px solid var(--border);
    border-radius: 4px;
    background: var(--paper);
    margin-bottom: 0.6rem;
  }
  #textViewer #textViewerClose { color: var(--danger); border-color: var(--danger); }
  #textViewer #textViewerClose:hover { background: var(--ink); color: var(--surface); border-color: var(--ink); }
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
  /* Name takes whatever width the other columns leave; a long AI title
     wraps under the filename instead of pushing the actions off-card. */
  td.name { white-space: nowrap; width: 100%; }
  td.actions { width: 1%; }
  @media (max-width: 560px) {
    td.date, th.date { display: none; }
  }
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
  #sd-widget {
    display: flex;
    justify-content: space-between;
    padding: 0.5rem 1rem;
    margin: 0 1rem 1.2rem;
    font-size: 0.78rem;
    color: var(--ink-soft);
  }
  #sd-widget b { color: var(--ink); font-weight: 600; }

  td.check, th.check { width: 1.6rem; text-align: center; padding-right: 0; }
  td.check input, th.check input { width: 1rem; height: 1rem; margin: 0; vertical-align: middle; }
  #batchBar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.6rem;
    padding: 0.6rem 1rem;
    margin: 0 1rem 1.2rem;
    font-size: 0.78rem;
  }
  #batchBar #batchCount { color: var(--ink-soft); white-space: nowrap; }
  #batchBar .actions { display: flex; gap: 0.4rem; flex-wrap: wrap; justify-content: flex-end; }
  #batchBar .actions button { width: auto; padding: 0.4rem 0.7rem; }
  #syncBar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.6rem;
    padding: 0.6rem 1rem;
    margin: 0 1rem 1.2rem;
    font-size: 0.78rem;
  }
  #syncBar #syncInfo { color: var(--ink-soft); }
  #syncBar button { white-space: nowrap; }
</style>
</head>
<body>
<div class="appbar">
  <div class="row">
    <h1>Annota</h1>
    <nav><a class="active" href="/" data-i18n="nav_files"></a><a href="/settings" data-i18n="nav_settings"></a></nav>
  </div>
  <div class="sub" data-i18n="sub_files"></div>
</div>

<div id="sd-widget" class="card" hidden>
  <span><span data-i18n="sd_used"></span> <b id="sdUsed">-</b></span>
  <span><span data-i18n="sd_free"></span> <b id="sdFree">-</b></span>
</div>

<div id="player" class="card" hidden>
  <div id="playerName"></div>
  <audio id="playerAudio" controls></audio>
  <button id="playerClose" class="btn">✕ <span data-i18n="stop"></span></button>
</div>

<div id="textViewer" class="card" hidden>
  <div id="textViewerName"></div>
  <div id="textViewerBody"></div>
  <button id="textViewerClose" class="btn">✕ <span data-i18n="close"></span></button>
</div>

<div id="syncBar" class="card">
  <span id="syncInfo"></span>
  <button id="syncBtn" data-i18n-title="sync_title">⟳ <span data-i18n="sync"></span></button>
</div>

<div id="batchBar" class="card" hidden>
  <span id="batchCount"></span>
  <div class="actions">
    <button id="batchDownload">⬇ <span data-i18n="download"></span></button>
    <button id="batchTranscribe">✎ <span data-i18n="transcribe"></span></button>
    <button id="batchDelete" class="danger">✕ <span data-i18n="delete"></span></button>
    <button id="batchClear" class="btn" data-i18n="clear_selection"></button>
  </div>
</div>

<div class="card" id="files-card">
  <div class="card-title" id="notesTitle"></div>
  <table id="files">
    <thead><tr>
      <th class="check"><input type="checkbox" id="selectAll" data-i18n-title="select_all"></th>
      <th class="sortable" data-sort="name"><span data-i18n="col_name"></span><span class="arrow"></span></th>
      <th class="sortable date" data-sort="mtime"><span data-i18n="col_date"></span><span class="arrow"></span></th>
      <th class="size" data-i18n="col_size"></th>
      <th class="actions"></th>
    </tr></thead>
    <tbody></tbody>
  </table>
</div>
<div id="empty" class="card" hidden></div>

<div class="card" id="others-card" hidden>
  <div class="card-title" data-i18n="other_files"></div>
  <table id="others"><tbody></tbody></table>
</div>

<div id="drop">
  <span data-i18n="drop_here"></span>
  <label class="btn"><span data-i18n="choose_one"></span><input id="picker" type="file" style="display:none"></label>
  <progress id="progress" max="100" value="0"></progress>
  <div id="status"></div>
</div>
)rawliteral";

// The browser-side Transcribe button's provider call (callProvider(key,
// blob, filename), returning { text, note? } or throwing) - the one
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
// Mirrors transcribe_openai.cpp's requests (whisper-1 multipart file
// upload, then a gpt-4o-mini chat completion for the title/abstract header)
// - talks to OpenAI directly from the browser instead of routing through
// the device.
async function summarizeOnce(key, text) {
  const res = await fetch("https://api.openai.com/v1/chat/completions", {
    method: "POST",
    headers: { "Authorization": "Bearer " + key, "Content-Type": "application/json" },
    body: JSON.stringify({
      model: "gpt-4o-mini",
      response_format: { type: "json_object" },
      messages: [
        { role: "system", content: "Given a transcript, reply with a JSON object {\"title\": string, \"abstract\": string}. " +
          "Title: short, at most 10 words. Abstract: 2 to 4 sentences summarizing the content. " +
          "Write both in the same language as the transcript." },
        { role: "user", content: text },
      ],
    }),
  });
  const json = await res.json();
  if (!res.ok) throw new Error((json.error && json.error.message) || ("HTTP " + res.status));
  const content = json.choices && json.choices[0] && json.choices[0].message && json.choices[0].message.content;
  if (typeof content !== "string") throw new Error("unexpected response");
  let out;
  try {
    out = JSON.parse(content);
  } catch (e) {
    throw new Error("model reply is not the expected JSON");
  }
  const title = typeof out.title === "string" ? out.title.trim() : "";
  const abstract = typeof out.abstract === "string" ? out.abstract.trim() : "";
  if (!title || !abstract) throw new Error("empty title/abstract");
  return { title, abstract };
}
// Two attempts, same as summarize_transcript()'s kMaxAttempts.
async function summarize(key, text) {
  try {
    return await summarizeOnce(key, text);
  } catch (e) {
    console.warn("Title/abstract attempt 1/2 failed:", e);
    await new Promise((r) => setTimeout(r, 1000));
    return summarizeOnce(key, text);
  }
}
// Phase keys (see PHASE_LABELS in INDEX_HTML_TAIL) this provider walks
// through, in order - numbers the "n/total" prefix like the device's
// progress screen does.
const PROVIDER_PHASES = ["download", "upload", "wait", "summarize", "save"];
// Returns { text, note } - note is set when the title/abstract header had
// to be skipped, so the caller can say so instead of silently saving a
// plain transcript.
async function callProvider(key, blob, filename) {
  const form = new FormData();
  form.append("model", "whisper-1");
  form.append("file", blob, filename);
  // XHR rather than fetch() - fetch has no upload progress events.
  const res = await xhrWithUploadProgress("POST", "https://api.openai.com/v1/audio/transcriptions",
    { "Authorization": "Bearer " + key }, form);
  const json = res.json;
  if (!res.ok) throw new Error((json && json.error && json.error.message) || ("HTTP " + res.status));
  // Same fallback as the device: a failed summary still saves the plain
  // transcript rather than losing the transcription.
  reportPhase("summarize");
  try {
    const s = await summarize(key, json.text);
    return { text: s.title + "\n\n" + s.abstract + "\n\n" + json.text };
  } catch (e) {
    console.warn("Title/abstract skipped:", e);
    return { text: json.text, note: t("note_no_summary", e.message) };
  }
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
// Phase keys (see PHASE_LABELS in INDEX_HTML_TAIL) this provider walks
// through, in order - no title/abstract step for Gemini.
const PROVIDER_PHASES = ["download", "upload", "wait", "save"];
async function callProvider(key, blob, filename) {
  const mimeType = /\.m4a$/i.test(filename) ? "audio/mp4" : "audio/mpeg";
  reportPhase("upload");
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
  const res = await xhrWithUploadProgress("POST", url, { "Content-Type": "application/json" }, JSON.stringify(body));
  const json = res.json || {};
  const parts = json.candidates && json.candidates[0] && json.candidates[0].content && json.candidates[0].content.parts;
  const text = parts && parts[0] && parts[0].text;
  if (!res.ok || typeof text !== "string") throw new Error((json.error && json.error.message) || t("err_unexpected_response", "Gemini"));
  return { text };
}
</script>
)rawliteral";
#endif

static const char INDEX_HTML_TAIL[] PROGMEM = R"rawliteral(
<script>
applyI18n(); // /i18n.js, loaded in <head> - see i18n.h

function fmtSize(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / (1024 * 1024)).toFixed(1) + " MB";
}

function fmtDate(mtime) {
  if (!mtime) return t("unknown_date");
  const d = new Date(mtime * 1000);
  return d.toLocaleString(window.I18N_LANG, { year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" });
}

let currentFiles = [];
// The page's two views of currentFiles (see buildNotes()): notes = one
// entry per audio file with its sibling transcript folded in, same as the
// device's Notes list; others = every file that's neither.
let notes = [];
let others = [];
// "default" = the device list's order (see noteBefore()); clicking the
// Name/Date headers cycles ascending -> descending -> back to default.
let sortKey = "default";
let sortDir = 1; // 1 = ascending, -1 = descending

// Batch selection - note (audio) filenames, not row/index references, so
// it survives a re-sort (render() rebuilds every row from scratch either
// way) and is simple to prune against notes after a refresh().
let selected = new Set();

// Keep in sync with storage.h's AUDIO_EXTS and web_server.cpp's
// audio_content_type() (every one gets a Play button and a working
// /api/play).
function isAudio(name) {
  return /\.(mp3|wav)$/i.test(name);
}

function isText(name) {
  return /\.txt$/i.test(name);
}

async function viewFile(name) {
  const status = document.getElementById("status");
  try {
    const res = await fetch("/api/download?name=" + encodeURIComponent(name));
    if (!res.ok) throw new Error(await res.text());
    const text = await res.text();
    document.getElementById("textViewerName").textContent = name;
    document.getElementById("textViewerBody").textContent = text;
    document.getElementById("player").hidden = true; // mutually exclusive with the audio player
    const viewer = document.getElementById("textViewer");
    viewer.hidden = false;
    viewer.scrollIntoView({ behavior: "smooth", block: "nearest" });
  } catch (e) {
    status.textContent = t("view_failed", e.message);
  }
}

document.getElementById("textViewerClose").onclick = () => {
  document.getElementById("textViewer").hidden = true;
};

function playFile(name) {
  const player = document.getElementById("player");
  const audio = document.getElementById("playerAudio");
  document.getElementById("playerName").textContent = name;
  audio.src = "/api/play?name=" + encodeURIComponent(name);
  document.getElementById("textViewer").hidden = true; // mutually exclusive with the text viewer
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

// Shows/hides #batchBar and keeps #selectAll's checked/indeterminate
// state in sync with `selected` - called at the end of render() (so sort
// clicks keep it in sync) and after every checkbox toggle.
function updateBatchBar() {
  document.getElementById("batchCount").textContent = t("n_selected", selected.size);
  document.getElementById("batchBar").hidden = selected.size === 0;
  const selectAll = document.getElementById("selectAll");
  const total = notes.length;
  selectAll.checked = total > 0 && selected.size === total;
  selectAll.indeterminate = selected.size > 0 && selected.size < total;
}

// Splits `files` (GET /api/files) into notes and others - see their
// declarations above. A transcript pairs with its audio file
// case-insensitively, since the SD card's FAT is.
function buildNotes(files) {
  const byName = new Map(files.map((f) => [f.name.toLowerCase(), f]));
  const paired = new Set();
  const notes = [];
  for (const f of files) {
    if (!isAudio(f.name)) continue;
    const transcript = byName.get(txtSibling(f.name).toLowerCase()) || null;
    if (transcript) paired.add(transcript.name);
    notes.push({ name: f.name, size: f.size, transcript });
  }
  const others = files.filter((f) => !isAudio(f.name) && !paired.has(f.name));
  return { notes, others };
}

function transcriptTime(n) {
  return n.transcript ? n.transcript.mtime : 0;
}

// Same order as the device list (storage.cpp's audio_before()):
// untranscribed first, then by transcript date, newest first. Ties keep
// the card's scan order (Array.prototype.sort is stable).
function noteBefore(a, b) {
  if (!a.transcript !== !b.transcript) return a.transcript ? 1 : -1;
  return transcriptTime(b) - transcriptTime(a);
}

// Icon-only action button (see td.actions' CSS comment) - title carries
// the label for a11y/tooltip instead of visible text, so four fit one row.
function actionButton(icon, title, onclick, danger) {
  const b = document.createElement("button");
  if (danger) b.className = "danger";
  b.textContent = icon;
  b.title = title;
  b.onclick = () => onclick(b);
  return b;
}

function downloadLink(name) {
  const dl = document.createElement("a");
  dl.className = "btn";
  dl.href = "/api/download?name=" + encodeURIComponent(name);
  dl.textContent = "⬇";
  dl.title = t("download");
  return dl;
}

function nameCell(icon, text, subtitle, subtitleNone) {
  const name = document.createElement("td");
  name.className = "name";
  const i = document.createElement("span");
  i.className = "file-icon";
  i.textContent = icon;
  name.appendChild(i);
  name.appendChild(document.createTextNode(text));
  if (subtitle) {
    const sub = document.createElement("div");
    sub.className = subtitleNone ? "note-title none" : "note-title";
    sub.textContent = subtitle;
    name.appendChild(sub);
  }
  return name;
}

function textCell(className, text) {
  const td = document.createElement("td");
  td.className = className;
  td.textContent = text;
  return td;
}

function render() {
  ({ notes, others } = buildNotes(currentFiles));

  // Drop selections for notes that no longer exist (deleted elsewhere,
  // or this is the refresh() after a batch action) - selected is keyed by
  // filename, so a stale entry would otherwise just sit there unseen.
  const stillPresent = new Set(notes.map((n) => n.name));
  for (const name of Array.from(selected)) {
    if (!stillPresent.has(name)) selected.delete(name);
  }

  const sorted = notes.slice().sort((a, b) => {
    if (sortKey === "default") return noteBefore(a, b);
    let cmp;
    if (sortKey === "name") {
      cmp = a.name.localeCompare(b.name, window.I18N_LANG, { sensitivity: "base" });
    } else {
      cmp = transcriptTime(a) - transcriptTime(b);
    }
    return cmp * sortDir;
  });

  document.querySelectorAll("th.sortable").forEach((th) => {
    const arrow = th.querySelector(".arrow");
    arrow.textContent = th.dataset.sort === sortKey ? (sortDir === 1 ? " ▲" : " ▼") : "";
  });

  document.getElementById("notesTitle").textContent = t("notes_title", notes.length);
  document.getElementById("files-card").hidden = notes.length === 0;
  const empty = document.getElementById("empty");
  empty.hidden = notes.length > 0;
  empty.textContent = currentFiles.length === 0 ? t("no_files") : t("no_notes");

  const body = document.querySelector("#files tbody");
  body.innerHTML = "";
  for (const n of sorted) {
    const tr = document.createElement("tr");

    const check = document.createElement("td");
    check.className = "check";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = selected.has(n.name);
    cb.onchange = () => {
      if (cb.checked) selected.add(n.name); else selected.delete(n.name);
      updateBatchBar();
    };
    check.appendChild(cb);
    tr.appendChild(check);

    // Second line: the transcript's AI title, nothing for a transcript
    // without one (plain-transcript fallback), or "no transcript yet".
    tr.appendChild(n.transcript ? nameCell("♪", n.name, n.transcript.title || "", false)
                                : nameCell("♪", n.name, t("no_transcript"), true));
    tr.appendChild(textCell("date", n.transcript ? fmtDate(n.transcript.mtime) : ""));
    tr.appendChild(textCell("size", fmtSize(n.size)));

    const actions = document.createElement("td");
    actions.className = "actions";
    actions.appendChild(actionButton("▶", t("play"), () => playFile(n.name)));
    // Same swap as the device's action menu: a transcribed note offers
    // View transcription in Transcribe's place.
    if (n.transcript) {
      actions.appendChild(actionButton("👁", t("view_transcript"), () => viewFile(n.transcript.name)));
    } else {
      actions.appendChild(actionButton("✎", t("transcribe"), (b) => transcribeFile(n.name, b)));
    }
    // Audio plus its transcript, if any - same pair Delete removes.
    actions.appendChild(actionButton("⬇", t("download"), () => downloadFiles(noteFiles(n))));
    actions.appendChild(actionButton("✕", t("delete"), () => removeNote(n), true));
    tr.appendChild(actions);
    body.appendChild(tr);
  }

  const othersSorted = others.slice().sort((a, b) =>
    a.name.localeCompare(b.name, window.I18N_LANG, { sensitivity: "base" }));
  document.getElementById("others-card").hidden = othersSorted.length === 0;
  const othersBody = document.querySelector("#others tbody");
  othersBody.innerHTML = "";
  for (const f of othersSorted) {
    const tr = document.createElement("tr");
    tr.appendChild(nameCell("☰", f.name));
    tr.appendChild(textCell("date", fmtDate(f.mtime)));
    tr.appendChild(textCell("size", fmtSize(f.size)));
    const actions = document.createElement("td");
    actions.className = "actions";
    if (isText(f.name)) actions.appendChild(actionButton("👁", t("view"), () => viewFile(f.name)));
    actions.appendChild(downloadLink(f.name));
    actions.appendChild(actionButton("✕", t("delete"), () => removeFile(f.name), true));
    tr.appendChild(actions);
    othersBody.appendChild(tr);
  }

  updateBatchBar();
  const pending = untranscribedAudio().length;
  document.getElementById("syncInfo").textContent = pending > 0 ? t("sync_pending", pending) : t("sync_nothing");
}

function fmtGb(bytes) { return (bytes / 1000000000).toFixed(2) + " GB"; }

// Reuses /api/settings rather than a dedicated endpoint - it already
// carries totalBytes/usedBytes for the Settings page's own SD card
// section (see SETTINGS_HTML's refresh()).
async function refreshSdInfo() {
  try {
    const res = await fetch("/api/settings");
    const info = await res.json();
    const widget = document.getElementById("sd-widget");
    if (!info.sdOk) { widget.hidden = true; return; }
    document.getElementById("sdUsed").textContent = fmtGb(info.usedBytes);
    document.getElementById("sdFree").textContent = fmtGb(info.totalBytes - info.usedBytes);
    widget.hidden = false;
  } catch (e) {
    document.getElementById("sd-widget").hidden = true;
  }
}

async function refresh() {
  const res = await fetch("/api/files");
  currentFiles = await res.json();
  render();
  refreshSdInfo();
}

document.querySelectorAll("th.sortable").forEach((th) => {
  th.onclick = () => {
    if (sortKey !== th.dataset.sort) {
      sortKey = th.dataset.sort;
      sortDir = 1;
    } else if (sortDir === 1) {
      sortDir = -1;
    } else {
      sortKey = "default";
      sortDir = 1;
    }
    render();
  };
});

// Transcription phase feedback - the web counterpart of the device's
// kTranscribeProgress screen (transcribe.cpp's phase_label()): a
// step-numbered phase line in #status, plus the #progress bar for the two
// phases that move bytes (download from the device, upload to the AI
// provider). PROVIDER_PHASES (TRANSCRIBE_PROVIDER_JS) picks which of these
// the compiled-in provider goes through, for the "n/total" numbering.
const PHASE_LABELS = {
  download: t("phase_download"),
  upload: t("phase_upload"),
  wait: t("phase_wait"),
  summarize: t("phase_summarize"),
  save: t("phase_save"),
};
let phaseTarget = ""; // file name (with a batch "[i/n] " prefix) shown after the phase

function reportPhase(phase, percent) {
  const idx = PROVIDER_PHASES.indexOf(phase);
  const step = idx >= 0 ? (idx + 1) + "/" + PROVIDER_PHASES.length + " " : "";
  let line = step + PHASE_LABELS[phase];
  if (percent !== undefined) line += " " + Math.round(percent) + "%";
  document.getElementById("status").textContent = line + " - " + phaseTarget;
  const bar = document.getElementById("progress");
  if (percent === undefined) {
    bar.style.display = "none";
  } else {
    bar.style.display = "block";
    bar.value = percent;
  }
}

function hidePhaseBar() {
  document.getElementById("progress").style.display = "none";
}

// GET via XHR (fetch has no portable download progress), reporting the
// "download" phase's percentage from /api/download's Content-Length.
function downloadWithProgress(url) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("GET", url);
    xhr.responseType = "blob";
    xhr.onprogress = (e) => { if (e.lengthComputable) reportPhase("download", (e.loaded / e.total) * 100); };
    xhr.onload = () => xhr.status === 200 ? resolve(xhr.response) : reject(new Error(t("err_download_http", xhr.status)));
    xhr.onerror = () => reject(new Error(t("err_download")));
    xhr.send();
  });
}

// Provider request helper for callProvider(): reports the "upload" phase's
// percentage while the body goes out, then flips to "wait" once the last
// byte is sent - same hand-off as the device's upload Stream. Resolves
// { ok, status, json } (json null if the reply isn't JSON).
function xhrWithUploadProgress(method, url, headers, body) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open(method, url);
    for (const h in headers) xhr.setRequestHeader(h, headers[h]);
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) reportPhase("upload", (e.loaded / e.total) * 100); };
    xhr.upload.onload = () => reportPhase("wait");
    xhr.onload = () => {
      let json = null;
      try { json = JSON.parse(xhr.responseText); } catch (e) {}
      resolve({ ok: xhr.status >= 200 && xhr.status < 300, status: xhr.status, json });
    };
    xhr.onerror = () => reject(new Error(t("err_ai_network")));
    reportPhase("upload", 0);
    xhr.send(body);
  });
}

// GET /api/transcript-key, split out of transcribeFile() so
// runTranscribeBatch() below can call it before each file of a batch - every
// call re-arms the device's web_transcribe_in_progress() deep-sleep guard,
// which the previous file's POST /api/transcript just cleared.
async function fetchTranscribeKey() {
  const keyRes = await fetch("/api/transcript-key");
  return keyRes.json(); // { key, providerName }
}

// Runs the transcription entirely from the browser: the audio bytes come
// from the existing /api/download, then whatever text comes back is
// stored via POST /api/transcript - the device itself never talks to the
// AI provider for this button. The actual provider call is callProvider(),
// defined just above in TRANSCRIBE_PROVIDER_JS - picked at build time by
// the same AI_PROVIDER_* flag that selects transcribe_<provider>.cpp on
// the device side, so this function itself has nothing provider-specific
// in it. `key` is fetched once by the caller (transcribeFile() or
// batchTranscribe() below) rather than in here, so a batch run doesn't
// re-fetch it per file.
async function transcribeOne(name, key, batchPrefix) {
  phaseTarget = (batchPrefix || "") + name;
  try {
    reportPhase("download");
    const audioBlob = await downloadWithProgress("/api/download?name=" + encodeURIComponent(name));
    const { text, note } = await callProvider(key, audioBlob, name);
    reportPhase("save");
    await saveTranscript(name, text);
    return note; // undefined unless the title/abstract header was skipped
  } finally {
    hidePhaseBar();
  }
}

async function saveTranscript(name, text) {
  const saveRes = await fetch("/api/transcript?name=" + encodeURIComponent(name), {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: text,
  });
  if (!saveRes.ok) throw new Error(t("err_save_transcript", await saveRes.text()));
}

async function transcribeFile(name, btn) {
  const status = document.getElementById("status");
  const icon = btn.textContent; // restored in finally - button stays icon-only, see td.actions' CSS comment
  btn.disabled = true;
  try {
    const { key, providerName } = await fetchTranscribeKey();
    if (!key) {
      alert(t("no_api_key", providerName));
      return;
    }

    btn.textContent = "…";
    const note = await transcribeOne(name, key);

    status.textContent = note ? t("transcribed_note", name, note) : t("transcribed", name);
    refresh();
  } catch (e) {
    status.textContent = t("transcribe_failed", e.message);
  } finally {
    btn.disabled = false;
    btn.textContent = icon;
  }
}

async function removeFile(name) {
  if (!confirm(t("confirm_delete", name))) return;
  const res = await fetch("/api/delete?name=" + encodeURIComponent(name), { method: "POST" });
  if (!res.ok) {
    alert(t("delete_failed", await res.text()));
  }
  refresh();
}

// A note's Delete removes its transcript too, same as on the device.
async function removeNote(n) {
  const msg = n.transcript ? t("confirm_delete_note", n.name, n.transcript.name) : t("confirm_delete", n.name);
  if (!confirm(msg)) return;
  for (const name of noteFiles(n)) {
    const res = await fetch("/api/delete?name=" + encodeURIComponent(name), { method: "POST" });
    if (!res.ok) {
      alert(t("delete_failed", await res.text()));
      break;
    }
  }
  refresh();
}

// A note's files on the card: the audio file, then its transcript if any.
function noteFiles(n) {
  return n.transcript ? [n.name, n.transcript.name] : [n.name];
}

function selectedNotes() {
  return notes.filter((n) => selected.has(n.name));
}

// Batch actions (#batchBar, wired up near the bottom of this script) - each
// just loops the same per-file request the single-row buttons above already
// use, sequentially, so nothing new is needed server-side.

// Triggers one browser download per file rather than a server-side
// zip (no zip library in this firmware, and SD-side zipping a possibly
// multi-file, multi-megabyte batch isn't worth adding one for). Staggered
// half a second apart - firing several a.click() downloads back-to-back in
// the same tick is what gets a browser's "this site is trying to download
// multiple files" block, or drops all but the first.
async function downloadFiles(names) {
  for (const name of names) {
    const a = document.createElement("a");
    a.href = "/api/download?name=" + encodeURIComponent(name);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    await new Promise((r) => setTimeout(r, 500));
  }
}

async function batchDownload() {
  await downloadFiles(selectedNotes().flatMap(noteFiles));
}

// Shared by batchTranscribe() and syncTranscribe(): transcribes `names` one
// after another. Both batch buttons stay disabled for the whole run so two
// batches can't overlap.
async function runTranscribeBatch(names) {
  const status = document.getElementById("status");
  const btns = [document.getElementById("batchTranscribe"), document.getElementById("syncBtn")];
  btns.forEach((b) => { b.disabled = true; });
  try {
    let failed = 0;
    let noSummary = 0;
    for (const [i, name] of names.entries()) {
      try {
        const { key, providerName } = await fetchTranscribeKey();
        if (!key) {
          alert(t("no_api_key", providerName));
          return;
        }
        if (await transcribeOne(name, key, "[" + (i + 1) + "/" + names.length + "] ")) noSummary++;
      } catch (e) {
        failed++;
        status.textContent = t("transcribe_failed_for", name, e.message);
      }
    }
    status.textContent = failed === 0 ? t("transcribed_n", names.length)
      : t("transcribed_partial", names.length - failed, names.length, failed);
    if (noSummary > 0) status.textContent += " " + t("saved_no_summary", noSummary);
  } finally {
    btns.forEach((b) => { b.disabled = false; });
    refresh();
  }
}

// Already-transcribed notes are silently skipped (their row offers View
// transcription instead of Transcribe too) rather than overwritten.
async function batchTranscribe() {
  const names = selectedNotes().filter((n) => !n.transcript).map((n) => n.name);
  if (names.length === 0) {
    alert(t("all_selected_transcribed"));
    return;
  }
  await runTranscribeBatch(names);
}

// "song.wav" -> "song.txt" - mirrors transcribe_openai.cpp's
// txt_sibling_path() and handle_save_transcript() on the device side.
function txtSibling(name) {
  const dot = name.lastIndexOf(".");
  return (dot >= 0 ? name.slice(0, dot) : name) + ".txt";
}

// Notes with no transcript yet (a leftover <basename>_error.txt doesn't
// count - those get retried).
function untranscribedAudio() {
  return notes
    .filter((n) => !n.transcript)
    .map((n) => n.name)
    .sort((a, b) => a.localeCompare(b, window.I18N_LANG, { sensitivity: "base" }));
}

// Sync button: transcribes every audio file on the card that has no
// transcript yet, through the same browser-side path as batchTranscribe().
async function syncTranscribe() {
  await refresh(); // the list may be stale (uploads/transcriptions from another tab)
  const names = untranscribedAudio();
  if (names.length === 0) {
    document.getElementById("status").textContent = t("sync_nothing");
    return;
  }
  if (!confirm(t("confirm_sync", names.length))) return;
  await runTranscribeBatch(names);
}

async function batchDelete() {
  const picked = selectedNotes();
  if (picked.length === 0) return;
  if (!confirm(t("confirm_delete_notes_n", picked.length))) return;
  const names = picked.flatMap(noteFiles);

  const status = document.getElementById("status");
  const btn = document.getElementById("batchDelete");
  btn.disabled = true;
  let failed = 0;
  for (const name of names) {
    status.textContent = t("deleting", name);
    try {
      const res = await fetch("/api/delete?name=" + encodeURIComponent(name), { method: "POST" });
      if (!res.ok) throw new Error(await res.text());
    } catch (e) {
      failed++;
      status.textContent = t("delete_failed_for", name, e.message);
    }
  }
  status.textContent = failed === 0 ? t("deleted_n", names.length)
    : t("deleted_partial", names.length - failed, names.length, failed);
  btn.disabled = false;
  refresh();
}

document.getElementById("selectAll").onchange = (e) => {
  if (e.target.checked) {
    notes.forEach((n) => selected.add(n.name));
  } else {
    selected.clear();
  }
  render();
};
document.getElementById("batchDownload").onclick = batchDownload;
document.getElementById("batchTranscribe").onclick = batchTranscribe;
document.getElementById("syncBtn").onclick = syncTranscribe;
document.getElementById("batchDelete").onclick = batchDelete;
document.getElementById("batchClear").onclick = () => { selected.clear(); render(); };

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
    status.textContent = xhr.status === 200 ? t("uploaded", file.name) : t("upload_failed_msg", xhr.responseText);
    refresh();
  };
  xhr.onerror = () => {
    progress.style.display = "none";
    status.textContent = t("upload_failed");
  };
  status.textContent = t("uploading", file.name);
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
<title data-i18n="title_settings"></title>
<script src="/i18n.js"></script>
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
  .pwd-wrap { position: relative; }
  .pwd-wrap input { padding-right: 2.4rem !important; }
  .pwd-toggle {
    position: absolute;
    right: 0.3rem;
    top: 50%;
    transform: translateY(-50%);
    width: auto;
    border: none;
    background: none;
    padding: 0.2rem 0.3rem;
    font-size: 0.95rem;
    line-height: 1;
    color: var(--ink-soft);
  }
  .pwd-toggle:hover { background: none; color: var(--ink); }
</style>
</head>
<body>
<div class="appbar">
  <div class="row">
    <h1>Annota</h1>
    <nav><a href="/" data-i18n="nav_files"></a><a class="active" href="/settings" data-i18n="nav_settings"></a></nav>
  </div>
  <div class="sub" data-i18n="sub_settings"></div>
</div>

<div class="card">
  <h2 data-i18n="language"></h2>
  <!-- Native names, deliberately not translated. -->
  <select id="langSelect"
    style="width:100%;padding:0.6rem 0.7rem;border-radius:4px;border:1.5px solid var(--border);background:var(--surface);color:var(--ink);font:inherit;box-sizing:border-box;">
    <option value="en">English</option>
    <option value="it">Italiano</option>
    <option value="fr">Français</option>
  </select>
</div>

<div class="card">
  <h2 data-i18n="status"></h2>
  <div class="row-item"><span class="label" data-i18n="wifi"></span><span class="value" id="wifiValue">-</span></div>
  <div class="row-item"><span class="label" data-i18n="clock"></span><span class="value" id="clockValue">-</span></div>
</div>

<div class="card">
  <h2 data-i18n="sd_card"></h2>
  <div class="row-item"><span class="label" data-i18n="capacity"></span><span class="value" id="cardValue">-</span></div>
  <div class="row-item"><span class="label" data-i18n="space_used"></span><span class="value" id="usedValue">-</span></div>
  <div class="bar"><div class="fill" id="usedBar" style="width:0%"></div></div>
  <div class="row-item" style="margin-top:0.4rem"><span class="label" data-i18n="audio_files"></span><span class="value" id="audioValue">-</span></div>
  <div class="row-item"><span class="label" data-i18n="text_files"></span><span class="value" id="textValue">-</span></div>
</div>

<div class="card">
  <h2 data-i18n="wifi_networks"></h2>
  <div id="networkList"></div>
  <div class="row-item" style="display:block;margin-top:0.6rem;padding-top:0.8rem;border-top:1px solid rgba(20,20,15,0.12);">
    <input id="newSsid" type="text" data-i18n-ph="ph_ssid"
      style="width:100%;margin-bottom:0.5rem;padding:0.6rem 0.7rem;border-radius:4px;border:1.5px solid var(--border);background:var(--surface);color:var(--ink);font:inherit;box-sizing:border-box;">
    <div class="pwd-wrap" style="margin-bottom:0.5rem;">
      <input id="newPass" type="password" data-i18n-ph="ph_password"
        style="width:100%;padding:0.6rem 0.7rem;border-radius:4px;border:1.5px solid var(--border);background:var(--surface);color:var(--ink);font:inherit;box-sizing:border-box;">
      <button type="button" class="pwd-toggle" id="newPassToggle" data-i18n-title="show_password">👁</button>
    </div>
    <button class="accent" id="addNetBtn">+ <span data-i18n="add_network"></span></button>
  </div>
  <button class="accent" id="reconnectBtn" style="margin-top:0.6rem;">↻ <span data-i18n="reconnect_wifi"></span></button>
</div>

<div class="card">
  <h2 data-i18n="power"></h2>
  <div class="row-item"><span class="label" data-i18n="sleep_after_idle"></span><span class="value" id="idleTimeoutValue">-</span></div>
  <input id="idleTimeoutSlider" type="range" min="1" max="180" step="1"
    style="width:100%;margin-top:0.6rem;">
</div>

<div class="card">
  <h2 id="keyTitle" data-i18n="ai_api_key"></h2>
  <div class="row-item"><span class="label" data-i18n="status"></span><span class="value" id="keyValue">-</span></div>
  <input id="keyInput" type="password" data-i18n-ph="ph_api_key"
    style="width:100%;margin-top:0.6rem;padding:0.6rem 0.7rem;border-radius:4px;border:1.5px solid var(--border);background:var(--surface);color:var(--ink);font:inherit;box-sizing:border-box;">
  <button class="accent" id="keySaveBtn" style="margin-top:0.6rem;" data-i18n="save_api_key"></button>
  <button class="danger" id="keyClearBtn" style="margin-top:0.6rem;">✕ <span data-i18n="clear_api_key"></span></button>
</div>

<div id="status"></div>

<script>
applyI18n(); // /i18n.js, loaded in <head> - see i18n.h

function fmtGb(bytes) { return (bytes / 1000000000).toFixed(2) + " GB"; }

async function refresh() {
  let info;
  try {
    const res = await fetch("/api/settings");
    info = await res.json();
  } catch (e) {
    document.getElementById("status").textContent = t("lost_connection");
    return;
  }

  const wifiValue = document.getElementById("wifiValue");
  if (info.wifiConnected) {
    wifiValue.textContent = info.ip;
    wifiValue.className = "value ok";
  } else {
    wifiValue.textContent = t("wifi_off");
    wifiValue.className = "value";
  }
  document.getElementById("reconnectBtn").disabled = info.wifiConnected;

  const clockValue = document.getElementById("clockValue");
  clockValue.textContent = info.clockSynced ? t("clock_synced") : t("clock_not_synced");
  clockValue.className = "value " + (info.clockSynced ? "ok" : "warn");

  const idleSlider = document.getElementById("idleTimeoutSlider");
  if (!idleSlider.matches(":active")) idleSlider.value = info.idleTimeoutMinutes; // don't yank it mid-drag
  document.getElementById("idleTimeoutValue").textContent = t("minutes", info.idleTimeoutMinutes);

  document.getElementById("keyTitle").textContent = t("provider_api_key", info.aiProviderName);
  const langSelect = document.getElementById("langSelect");
  if (document.activeElement !== langSelect) langSelect.value = info.language;
  const keyValue = document.getElementById("keyValue");
  keyValue.textContent = info.aiKeyConfigured ? t("key_set") : t("key_not_set");
  keyValue.className = "value " + (info.aiKeyConfigured ? "ok" : "warn");

  if (info.sdOk) {
    document.getElementById("cardValue").textContent = fmtGb(info.cardBytes);
    const usedPct = info.totalBytes > 0 ? (100 * info.usedBytes / info.totalBytes) : 0;
    document.getElementById("usedValue").textContent = usedPct.toFixed(1) + "%";
    document.getElementById("usedBar").style.width = usedPct.toFixed(1) + "%";
    document.getElementById("audioValue").textContent = info.audioFileCount;
    document.getElementById("textValue").textContent = info.textFileCount;
  } else {
    document.getElementById("cardValue").textContent = t("unavailable");
    document.getElementById("usedValue").textContent = "-";
    document.getElementById("audioValue").textContent = "-";
    document.getElementById("textValue").textContent = "-";
  }
}

function renderNetworks(ssids) {
  const container = document.getElementById("networkList");
  container.innerHTML = "";
  if (ssids.length === 0) {
    container.innerHTML = '<div class="row-item"><span class="label"></span></div>';
    container.querySelector(".label").textContent = t("no_networks");
    return;
  }
  ssids.forEach((ssid) => {
    const row = document.createElement("div");
    row.className = "row-item";
    const label = document.createElement("span");
    label.className = "label";
    label.textContent = ssid;
    const actions = document.createElement("span");
    const editBtn = document.createElement("button");
    editBtn.textContent = t("edit");
    editBtn.style.cssText = "width:auto;padding:0.3rem 0.6rem;";
    editBtn.onclick = () => startEditNetwork(row, ssid);
    const removeBtn = document.createElement("button");
    removeBtn.className = "danger";
    removeBtn.textContent = t("remove");
    removeBtn.style.cssText = "width:auto;padding:0.3rem 0.6rem;margin-left:0.4rem;";
    removeBtn.onclick = () => removeNetwork(ssid);
    actions.appendChild(editBtn);
    actions.appendChild(removeBtn);
    row.appendChild(label);
    row.appendChild(actions);
    container.appendChild(row);
  });
}

function togglePwd(input, btn) {
  const show = input.type === "password";
  input.type = show ? "text" : "password";
  btn.textContent = show ? "🙈" : "👁";
  btn.title = show ? t("hide_password") : t("show_password");
  btn.setAttribute("aria-label", btn.title);
}

function startEditNetwork(row, ssid) {
  row.innerHTML = "";
  const wrap = document.createElement("div");
  wrap.className = "pwd-wrap";
  wrap.style.cssText = "flex:1;margin-right:0.5rem;";
  const input = document.createElement("input");
  input.type = "password";
  input.placeholder = t("ph_new_password");
  input.style.cssText = "width:100%;";
  const toggleBtn = document.createElement("button");
  toggleBtn.type = "button";
  toggleBtn.className = "pwd-toggle";
  toggleBtn.textContent = "👁";
  toggleBtn.title = t("show_password");
  toggleBtn.setAttribute("aria-label", toggleBtn.title);
  toggleBtn.onclick = () => togglePwd(input, toggleBtn);
  wrap.appendChild(input);
  wrap.appendChild(toggleBtn);
  const saveBtn = document.createElement("button");
  saveBtn.textContent = t("save");
  saveBtn.style.cssText = "width:auto;";
  saveBtn.onclick = async () => {
    if (!input.value) return; // blank = no-op, same guard as keySaveBtn
    const form = new URLSearchParams();
    form.set("ssid", ssid);
    form.set("password", input.value);
    const res = await fetch("/api/wifi/networks/update", { method: "POST", body: form });
    document.getElementById("status").textContent = res.ok ? t("network_updated") : t("update_failed", await res.text());
    loadNetworks();
  };
  const cancelBtn = document.createElement("button");
  cancelBtn.textContent = t("cancel");
  cancelBtn.style.cssText = "width:auto;margin-left:0.4rem;";
  cancelBtn.onclick = loadNetworks;
  row.appendChild(wrap);
  row.appendChild(saveBtn);
  row.appendChild(cancelBtn);
}

async function removeNetwork(ssid) {
  if (!confirm(t("confirm_remove_network", ssid))) return;
  const form = new URLSearchParams();
  form.set("ssid", ssid);
  const res = await fetch("/api/wifi/networks/remove", { method: "POST", body: form });
  document.getElementById("status").textContent = res.ok ? t("network_removed") : t("remove_failed", await res.text());
  loadNetworks();
}

async function loadNetworks() {
  const res = await fetch("/api/wifi/networks");
  const data = await res.json();
  renderNetworks(data.networks);
}

document.getElementById("newPassToggle").onclick = () => togglePwd(
  document.getElementById("newPass"), document.getElementById("newPassToggle"));

document.getElementById("addNetBtn").onclick = async () => {
  const ssidInput = document.getElementById("newSsid");
  const passInput = document.getElementById("newPass");
  if (!ssidInput.value) return;
  const form = new URLSearchParams();
  form.set("ssid", ssidInput.value);
  form.set("password", passInput.value);
  const res = await fetch("/api/wifi/networks", { method: "POST", body: form });
  document.getElementById("status").textContent = res.ok ? t("network_added") : t("add_failed", await res.text());
  ssidInput.value = "";
  passInput.value = "";
  loadNetworks();
};

let pollTimer = null;

document.getElementById("reconnectBtn").onclick = async () => {
  const btn = document.getElementById("reconnectBtn");
  const status = document.getElementById("status");
  btn.disabled = true;
  status.textContent = t("reconnecting");
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

// "input" fires continuously while dragging (live label, no request);
// "change" fires once on release, which is when it's actually saved -
// a slider's own natural "Save" moment, no separate button needed.
document.getElementById("idleTimeoutSlider").oninput = (e) => {
  document.getElementById("idleTimeoutValue").textContent = t("minutes", e.target.value);
};
document.getElementById("idleTimeoutSlider").onchange = async (e) => {
  const status = document.getElementById("status");
  const form = new URLSearchParams();
  form.set("minutes", e.target.value);
  const res = await fetch("/api/settings/idle-timeout", { method: "POST", body: form });
  status.textContent = res.ok ? t("sleep_timeout_updated") : t("update_failed", await res.text());
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
  status.textContent = res.ok ? t("api_key_saved") : t("save_failed", await res.text());
  refresh();
};

document.getElementById("keyClearBtn").onclick = async () => {
  if (!confirm(t("confirm_clear_key"))) return;
  const status = document.getElementById("status");
  const form = new URLSearchParams();
  form.set("key", "");
  const res = await fetch("/api/settings/ai-key", { method: "POST", body: form });
  status.textContent = res.ok ? t("api_key_cleared") : t("clear_failed", await res.text());
  refresh();
};

// Saved server-side, then a reload picks up the new /i18n.js - simpler
// than re-rendering every string in place, and the page has no state
// worth keeping across it.
document.getElementById("langSelect").onchange = async (e) => {
  const form = new URLSearchParams();
  form.set("lang", e.target.value);
  const res = await fetch("/api/settings/language", { method: "POST", body: form });
  if (res.ok) {
    location.reload();
  } else {
    document.getElementById("status").textContent = t("update_failed", await res.text());
  }
};

refresh();
loadNetworks();
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

// GET /i18n.js - the current language's web strings plus the t()/
// applyI18n() helpers (i18n.h's i18n_write_web_js()), loaded
// synchronously from both pages' <head>. no-store so a language change
// shows up on the very next page load.
static void handle_i18n_js() {
    String out;
    i18n_write_web_js(out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/javascript; charset=utf-8", out);
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
    doc["idleTimeoutMinutes"] = sleep_get_idle_timeout_minutes();
    doc["language"] = i18n_lang_code(i18n_get_language());

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

// POST /api/settings/idle-timeout - saves the idle-sleep slider from
// SETTINGS_HTML. sleep_set_idle_timeout_minutes() itself clamps to
// [1, 180], so an out-of-range value here just gets silently clamped
// rather than rejected - the slider's own min/max already keeps normal
// use inside that range, this is only a backstop against a hand-crafted
// request. Takes effect immediately (no reboot) via
// sleep_process_idle()'s next call.
static void handle_settings_set_idle_timeout() {
    if (!server.hasArg("minutes")) {
        server.send(400, "text/plain", "Missing minutes");
        return;
    }
    long minutes = server.arg("minutes").toInt();
    if (minutes <= 0) {
        server.send(400, "text/plain", "Invalid minutes");
        return;
    }
    sleep_set_idle_timeout_minutes((uint16_t)minutes);
    server.send(200, "text/plain", "OK");
}

// POST /api/settings/language - saves the Settings page's language picker
// (i18n.h's i18n_set_language(): "en"/"it"/"fr"). Applies to both the web
// pages (on their next load) and the e-paper UI, which repaints on the
// next loop() pass via ui_request_rerender() - no reboot needed.
static void handle_settings_set_language() {
    if (!server.hasArg("lang")) {
        server.send(400, "text/plain", "Missing lang");
        return;
    }
    Lang lang;
    if (!i18n_parse_lang(server.arg("lang").c_str(), lang)) {
        server.send(400, "text/plain", "Unsupported lang");
        return;
    }
    i18n_set_language(lang);
    ui_request_rerender();
    server.send(200, "text/plain", "OK");
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

// GET /api/wifi/networks - SSID-only list for the Settings page's WiFi
// Networks card. Never returns a password - same "never echo a saved
// secret back" rule handle_settings_info() applies to the AI key.
static void handle_wifi_list() {
    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    char ssid[WIFI_SSID_MAX_LEN + 1];
    int count = wifi_saved_network_count();
    for (int i = 0; i < count; i++) {
        if (wifi_get_saved_network_ssid(i, ssid, sizeof(ssid))) arr.add(ssid);
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// POST /api/wifi/networks (ssid, password) - adds a network to the saved
// list (wifi_manager.h). password may be empty (open network). Rejects a
// duplicate ssid (use the update route instead), a full list, or
// oversized fields.
static void handle_wifi_add() {
    if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
        server.send(400, "text/plain", "Missing ssid");
        return;
    }
    String ssid = server.arg("ssid");
    String password = server.hasArg("password") ? server.arg("password") : "";
    if (ssid.length() > WIFI_SSID_MAX_LEN) {
        server.send(400, "text/plain", "SSID too long");
        return;
    }
    if (password.length() > WIFI_PASSWORD_MAX_LEN) {
        server.send(400, "text/plain", "Password too long");
        return;
    }
    bool ok = wifi_add_network(ssid.c_str(), password.c_str());
    server.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Already saved or list full");
}

// POST /api/wifi/networks/update (ssid, password) - the Edit action: retype-
// only, same convention as the AI key field (never prefilled, blank means
// no-op) - SETTINGS_HTML's startEditNetwork() never submits this with a
// blank password, so a missing/empty one here is treated as a bad request
// rather than "keep existing".
static void handle_wifi_update() {
    if (!server.hasArg("ssid") || !server.hasArg("password") || server.arg("password").length() == 0) {
        server.send(400, "text/plain", "Missing ssid or password");
        return;
    }
    if (server.arg("password").length() > WIFI_PASSWORD_MAX_LEN) {
        server.send(400, "text/plain", "Password too long");
        return;
    }
    bool ok = wifi_update_network_password(server.arg("ssid").c_str(), server.arg("password").c_str());
    server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Not found");
}

// POST /api/wifi/networks/remove (ssid).
static void handle_wifi_remove() {
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "Missing ssid");
        return;
    }
    bool ok = wifi_remove_network(server.arg("ssid").c_str());
    server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Not found");
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
// Safety-net cap on web_transcribe_in_progress() - if the browser never
// calls handle_save_transcript() back (tab closed, network drop mid-
// upload), this is what lets the device fall asleep again eventually
// instead of staying pinned awake forever. Generous on purpose - a real
// transcription of a longer recording can legitimately take minutes.
static const uint32_t WEB_TRANSCRIBE_MAX_MS = 300000UL;
static uint32_t webTranscribeDeadlineMs = 0; // 0 = no transcription in flight

bool web_transcribe_in_progress() {
    return webTranscribeDeadlineMs != 0 && (int32_t)(millis() - webTranscribeDeadlineMs) < 0;
}

static void handle_get_transcript_key() {
    webTranscribeDeadlineMs = millis() + WEB_TRANSCRIBE_MAX_MS;

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
    webTranscribeDeadlineMs = 0; // round trip is finishing (success or not) - see web_transcribe_in_progress()

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
                // A transcript's AI title, shown under its audio file in
                // the page's Notes table.
                size_t baseLen = strlen(base);
                if (baseLen > 4 && strcasecmp(base + baseLen - 4, ".txt") == 0) {
                    char title[128];
                    if (read_transcript_title(entry, title, sizeof(title))) o["title"] = title;
                }
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
    if (ends_with(".mp3")) return "audio/mpeg";
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
    static bool serverStarted = false;
    if (serverStarted) return;
    serverStarted = true;

    server.on("/", HTTP_GET, with_activity(handle_root));
    server.on("/settings", HTTP_GET, with_activity(handle_settings_page));
    server.on("/api/settings", HTTP_GET, with_activity(handle_settings_info));
    server.on("/api/settings/reconnect", HTTP_POST, with_activity(handle_settings_reconnect));
    server.on("/api/settings/ai-key", HTTP_POST, with_activity(handle_settings_set_ai_key));
    server.on("/api/settings/idle-timeout", HTTP_POST, with_activity(handle_settings_set_idle_timeout));
    server.on("/api/settings/language", HTTP_POST, with_activity(handle_settings_set_language));
    server.on("/i18n.js", HTTP_GET, with_activity(handle_i18n_js));
    server.on("/api/wifi/networks", HTTP_GET, with_activity(handle_wifi_list));
    server.on("/api/wifi/networks", HTTP_POST, with_activity(handle_wifi_add));
    server.on("/api/wifi/networks/update", HTTP_POST, with_activity(handle_wifi_update));
    server.on("/api/wifi/networks/remove", HTTP_POST, with_activity(handle_wifi_remove));
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
