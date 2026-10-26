#include "transcribe_upload.h"

String tls_error_suffix(WiFiClientSecure &client) {
    char buf[96];
    if (client.lastError(buf, sizeof(buf)) == 0 || buf[0] == '\0') return String();
    return String(", TLS: ") + buf;
}

const char *upload_mime_type_for(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (dot && strcasecmp(dot, ".wav") == 0) return "audio/wav";
    if (dot && strcasecmp(dot, ".m4a") == 0) return "audio/mp4";
    return "audio/mpeg";
}

void txt_sibling_path(const char *filename, char *out, size_t outLen) {
    const char *dot = strrchr(filename, '.');
    size_t baseLen = dot ? (size_t)(dot - filename) : strlen(filename);
    if (baseLen > outLen - 6) baseLen = outLen - 6; // '/' + up to baseLen + ".txt" + '\0'
    out[0] = '/';
    memcpy(out + 1, filename, baseLen);
    strcpy(out + 1 + baseLen, ".txt");
}

void set_err(char *errOut, size_t errOutLen, const char *msg) {
    strncpy(errOut, msg, errOutLen - 1);
    errOut[errOutLen - 1] = '\0';
}
