#include "web_server.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include "display.h"
#include "storage.h"

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

// Claims the shared SPI peripheral for one request: pause touch, then mount
// the card. Pair every successful call with sd_release().
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

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Annota - SD files</title>
<style>
  /* Palette lifted straight from the device's own screen (ui.cpp): bg
     0x14161C, card bg 0x2A2E3A, radius 12px, secondary text 0x9AA0AC. */
  :root {
    color-scheme: dark;
    --bg: #14161c;
    --surface: #2a2e3a;
    --surface-2: #343947;
    --on-surface: #ffffff;
    --on-surface-secondary: #9aa0ac;
    --accent: #3498db;
    --danger: #e06a5a;
    --divider: rgba(255, 255, 255, 0.08);
  }
  * { box-sizing: border-box; }
  body {
    font-family: "Roboto", -apple-system, system-ui, sans-serif;
    background: var(--bg);
    color: var(--on-surface);
    max-width: 640px;
    margin: 0 auto;
    padding: 0 0 2rem;
  }
  .appbar {
    padding: 1.1rem 1rem;
    margin-bottom: 1rem;
    box-shadow: 0 2px 6px rgba(0, 0, 0, 0.4);
  }
  .appbar h1 {
    margin: 0;
    font-size: 1.15rem;
    font-weight: 500;
    letter-spacing: 0.01em;
  }
  .appbar .sub { color: var(--on-surface-secondary); font-size: 0.8rem; margin-top: 0.15rem; }
  .card {
    background: var(--surface);
    border-radius: 12px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.4), 0 1px 2px rgba(0, 0, 0, 0.3);
    margin: 0 1rem 1.2rem;
    overflow: hidden;
  }
  #player { padding: 0.9rem 1rem; }
  #player #playerName { font-size: 0.85rem; margin-bottom: 0.5rem; word-break: break-all; }
  #player audio { width: 100%; height: 32px; }
  #player #playerClose { margin-top: 0.4rem; color: var(--danger); }
  #player #playerClose:hover { background: rgba(224, 106, 90, 0.15); }
  td.play { white-space: nowrap; }
  table { width: 100%; border-collapse: collapse; }
  th, td { text-align: left; padding: 0.7rem 0.8rem; }
  thead th {
    font-size: 0.72rem;
    font-weight: 500;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--on-surface-secondary);
    border-bottom: 1px solid var(--divider);
  }
  tbody tr { border-bottom: 1px solid var(--divider); }
  tbody tr:last-child { border-bottom: none; }
  tbody tr:hover { background: rgba(255, 255, 255, 0.03); }
  th.sortable { cursor: pointer; user-select: none; white-space: nowrap; }
  th.sortable:hover { color: var(--on-surface); }
  .arrow { color: var(--accent); }
  td.size, th.size { text-align: right; white-space: nowrap; }
  td.date, th.date { white-space: nowrap; color: var(--on-surface-secondary); font-size: 0.85rem; }
  td.actions { text-align: right; white-space: nowrap; }

  button, .btn {
    font: inherit;
    font-weight: 500;
    font-size: 0.78rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    cursor: pointer;
    border: none;
    background: transparent;
    color: var(--accent);
    padding: 0.4rem 0.6rem;
    border-radius: 6px;
    transition: background 0.15s ease;
  }
  button:hover, .btn:hover { background: rgba(52, 152, 219, 0.15); }
  button.danger { color: var(--danger); }
  button.danger:hover { background: rgba(224, 106, 90, 0.15); }

  #drop {
    margin: 0 1rem 1.2rem;
    padding: 1.6rem 1rem;
    border: 2px dashed rgba(255, 255, 255, 0.15);
    border-radius: 12px;
    text-align: center;
    color: var(--on-surface-secondary);
  }
  #drop.over { border-color: var(--accent); background: rgba(52, 152, 219, 0.06); }
  #drop .btn { background: var(--surface-2); color: var(--on-surface); display: inline-block; margin-top: 0.4rem; }
  #drop .btn:hover { background: #3d4353; }
  #status { margin-top: 0.7rem; font-size: 0.85rem; color: var(--on-surface-secondary); }
  progress {
    width: 100%;
    margin-top: 0.7rem;
    display: none;
    height: 6px;
    border-radius: 3px;
    overflow: hidden;
    border: none;
  }
  progress::-webkit-progress-bar { background: var(--surface-2); border-radius: 3px; }
  progress::-webkit-progress-value { background: var(--accent); border-radius: 3px; }
  progress::-moz-progress-bar { background: var(--accent); border-radius: 3px; }
  #empty { color: var(--on-surface-secondary); text-align: center; padding: 1.2rem; margin: 0 1rem 1.2rem; }
</style>
</head>
<body>
<div class="appbar">
  <h1>Annota</h1>
  <div class="sub">SD card files</div>
</div>

<div id="player" class="card" hidden>
  <div id="playerName"></div>
  <audio id="playerAudio" controls></audio>
  <button id="playerClose" class="btn">Stop</button>
</div>

<div class="card">
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

// Keep in sync with web_server.cpp's audio_content_type() - only these
// extensions get a Play button and a working /api/play.
function isAudio(name) {
  return /\.(mp3|wav|ogg|m4a|aac|flac)$/i.test(name);
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
    name.textContent = f.name;
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

    if (isAudio(f.name)) {
      const play = document.createElement("button");
      play.textContent = "Play";
      play.onclick = () => playFile(f.name);
      actions.appendChild(play);
      actions.appendChild(document.createTextNode(" "));
    }

    const dl = document.createElement("a");
    dl.className = "btn";
    dl.href = "/api/download?name=" + encodeURIComponent(f.name);
    dl.textContent = "Download";
    actions.appendChild(dl);

    actions.appendChild(document.createTextNode(" "));

    const del = document.createElement("button");
    del.className = "danger";
    del.textContent = "Delete";
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

static void handle_root() {
    server.send_P(200, "text/html", INDEX_HTML);
}

static void handle_list() {
    if (!sd_claim()) {
        server.send(503, "text/plain", "SD card not available");
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    File root = SD.open("/");
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
    if (ends_with(".mp3")) return "audio/mpeg";
    if (ends_with(".wav")) return "audio/wav";
    if (ends_with(".ogg")) return "audio/ogg";
    if (ends_with(".m4a")) return "audio/mp4";
    if (ends_with(".aac")) return "audio/aac";
    if (ends_with(".flac")) return "audio/flac";
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
    if (!SD.exists(path)) {
        sd_release();
        server.send(404, "text/plain", "Not found");
        return;
    }

    File f = SD.open(path, FILE_READ);
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
    if (!SD.exists(path)) {
        sd_release();
        server.send(404, "text/plain", "Not found");
        return;
    }

    File f = SD.open(path, FILE_READ);
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
    bool ok = SD.remove(path);
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
        uploadFile = SD.open(path, FILE_WRITE);
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

void web_server_start() {
    server.on("/", HTTP_GET, handle_root);
    server.on("/api/files", HTTP_GET, handle_list);
    server.on("/api/download", HTTP_GET, handle_download);
    server.on("/api/play", HTTP_GET, handle_play);
    server.on("/api/delete", HTTP_POST, handle_delete);
    server.on("/api/upload", HTTP_POST, handle_upload_done, handle_upload_data);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();

    Serial.print("Web file manager: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
}

void web_server_handle() {
    server.handleClient();
}
