#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(ASTRALWEBM_NO_FCGI) && defined(__has_include)
#if __has_include(<fcgiapp.h>)
#include <fcgiapp.h>
#define ASTRALWEBM_HAS_FCGI 1
#endif
#endif

#ifndef ASTRALWEBM_HAS_FCGI
#include <unistd.h>
extern char **environ;
typedef FILE FCGX_Stream;
typedef struct {
    FCGX_Stream *in;
    FCGX_Stream *out;
    FCGX_Stream *err;
    char **envp;
} FCGX_Request;

static int FCGX_Init(void) { return 0; }
static int FCGX_OpenSocket(const char *path, int backlog) {
    (void)path;
    (void)backlog;
    return 0;
}
static int FCGX_InitRequest(FCGX_Request *request, int sock, int flags) {
    (void)sock;
    (void)flags;
    request->in = stdin;
    request->out = stdout;
    request->err = stderr;
    request->envp = environ;
    return 0;
}
static int FCGX_Accept_r(FCGX_Request *request) {
    static int accepted_once = 0;
    if (accepted_once) {
        return -1;
    }
    accepted_once = 1;
    request->in = stdin;
    request->out = stdout;
    request->err = stderr;
    request->envp = environ;
    return 0;
}
static void FCGX_Finish_r(FCGX_Request *request) { (void)request; }
static const char *FCGX_GetParam(const char *name, char **envp) {
    (void)envp;
    return getenv(name);
}
static int FCGX_FPrintF(FCGX_Stream *stream, const char *fmt, ...) {
    int rc;
    va_list ap;
    va_start(ap, fmt);
    rc = vfprintf(stream, fmt, ap);
    va_end(ap);
    return rc;
}
static int FCGX_PutStr(const char *str, int len, FCGX_Stream *stream) {
    return (fwrite(str, 1U, (size_t)len, stream) == (size_t)len) ? len : -1;
}
static int FCGX_FFlush(FCGX_Stream *stream) { return fflush(stream); }
#endif

#define APP_NAME "astralwebm"
#define MAX_SESSIONS 3
#define TOKEN_BYTES 16
#define TOKEN_HEX_LEN (TOKEN_BYTES * 2)

#define DEFAULT_CAMERA 1
#define MAX_CAMERA 99
#define DEFAULT_WIDTH 800
#define DEFAULT_FPS 1
#define DEFAULT_TIMEOUT_SEC 120
#define MAX_TIMEOUT_SEC 300
#define TOKEN_TTL_SEC 30

#define BITRATE_1FPS_KBPS 180
#define BITRATE_5FPS_KBPS 450
#define BITRATE_10FPS_KBPS 900

typedef struct {
    bool in_use;
    bool active;
    char token[TOKEN_HEX_LEN + 1];
    int camera;
    int width;
    int fps;
    int bitrate_kbps;
    int timeout_sec;
    time_t created_at;
    time_t expires_at;
    time_t started_at;
} stream_session_t;

static stream_session_t g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

/* Keep this fallback in sync with html/index.html. */
static const char *g_embedded_html =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>astralwebm</title>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}"
    ".wrap{max-width:960px;margin:0 auto}video{width:100%;background:#000}"
    ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:12px 0}</style>"
    "</head><body><div class=\"wrap\"><h1>astralwebm</h1>"
    "<p>On-demand WebM stream. Press Play to request a short-lived token first.</p>"
    "<div class=\"row\"><label>Camera <input id=\"camera\" type=\"number\" min=\"1\" value=\"1\"></label>"
    "<label>FPS <select id=\"fps\"><option value=\"1\" selected>1</option><option value=\"5\">5</option>"
    "<option value=\"10\">10</option></select></label>"
    "<button id=\"play\">Play</button><button id=\"stop\">Stop</button></div>"
    "<video id=\"player\" controls playsinline></video><pre id=\"status\"></pre></div>"
    "<script>"
    "const p=document.getElementById('player');const s=document.getElementById('status');"
    "document.getElementById('play').addEventListener('click',async()=>{"
    "const cam=parseInt(document.getElementById('camera').value||'1',10)||1;"
    "const fps=document.getElementById('fps').value;"
    "s.textContent='Requesting session...';"
    "try{const r=await fetch(`session/start?camera=${cam}&fps=${fps}`,{method:'POST'});"
    "if(!r.ok){s.textContent='Session start failed: '+(await r.text());return;}"
    "const j=await r.json();p.src=j.streamUrl;s.textContent='Streaming...';await p.play();"
    "}catch(e){s.textContent='Error: '+e.message;}});"
    "document.getElementById('stop').addEventListener('click',()=>{p.pause();p.removeAttribute('src');p.load();s.textContent='Stopped';});"
    "</script></body></html>";

static int bitrate_for_fps(int fps) {
    switch (fps) {
        case 1:
            return BITRATE_1FPS_KBPS;
        case 5:
            return BITRATE_5FPS_KBPS;
        case 10:
            return BITRATE_10FPS_KBPS;
        default:
            return BITRATE_1FPS_KBPS;
    }
}

static bool is_allowed_fps(int fps) {
    return fps == 1 || fps == 5 || fps == 10;
}

static int clamp_timeout(int timeout_sec) {
    if (timeout_sec <= 0) {
        return DEFAULT_TIMEOUT_SEC;
    }
    if (timeout_sec > MAX_TIMEOUT_SEC) {
        return MAX_TIMEOUT_SEC;
    }
    return timeout_sec;
}

static bool query_param_value(const char *query, const char *key, char *out, size_t out_len) {
    size_t key_len;
    const char *p;

    if (!query || !key || !out || out_len == 0) {
        return false;
    }

    key_len = strlen(key);
    p = query;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        size_t param_len;
        if (!eq) {
            break;
        }
        if (!amp) {
            amp = p + strlen(p);
        }
        param_len = (size_t)(eq - p);
        if (param_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = (size_t)(amp - eq - 1);
            if (val_len >= out_len) {
                val_len = out_len - 1;
            }
            memcpy(out, eq + 1, val_len);
            out[val_len] = '\0';
            return true;
        }
        if (*amp == '\0') {
            break;
        }
        p = amp + 1;
    }
    return false;
}

static bool parse_positive_int(const char *query, const char *key, int *value_out) {
    char buf[32];
    char *end = NULL;
    long val;

    if (!query_param_value(query, key, buf, sizeof(buf))) {
        return false;
    }

    errno = 0;
    val = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0' || val <= 0 || val > INT_MAX) {
        return false;
    }

    *value_out = (int)val;
    return true;
}

static int generate_token(char out[TOKEN_HEX_LEN + 1]) {
    uint8_t raw[TOKEN_BYTES];
    FILE *f = fopen("/dev/urandom", "rb");
    size_t i;

    if (!f) {
        return -1;
    }
    if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    for (i = 0; i < sizeof(raw); ++i) {
        (void)snprintf(out + (i * 2), 3, "%02x", raw[i]);
    }
    out[TOKEN_HEX_LEN] = '\0';
    return 0;
}

static void release_session_locked(size_t idx) {
    memset(&g_sessions[idx], 0, sizeof(g_sessions[idx]));
}

static int create_session(int camera, int fps, int timeout_sec, char token_out[TOKEN_HEX_LEN + 1]) {
    size_t i;
    int slot = -1;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_sessions_lock);
    for (i = 0; i < MAX_SESSIONS; ++i) {
        if (g_sessions[i].in_use && g_sessions[i].expires_at < now && !g_sessions[i].active) {
            release_session_locked(i);
        }
    }
    for (i = 0; i < MAX_SESSIONS; ++i) {
        if (!g_sessions[i].in_use) {
            slot = (int)i;
            break;
        }
    }
    if (slot >= 0 && generate_token(token_out) == 0) {
        stream_session_t *session = &g_sessions[slot];
        memset(session, 0, sizeof(*session));
        session->in_use = true;
        session->active = false;
        memcpy(session->token, token_out, TOKEN_HEX_LEN + 1);
        session->camera = camera;
        session->width = DEFAULT_WIDTH;
        session->fps = fps;
        session->bitrate_kbps = bitrate_for_fps(fps);
        session->timeout_sec = timeout_sec;
        session->created_at = now;
        session->expires_at = now + TOKEN_TTL_SEC;
    } else {
        slot = -1;
    }
    pthread_mutex_unlock(&g_sessions_lock);

    return slot;
}

static bool activate_session_by_token(const char *token, stream_session_t *session_out, size_t *idx_out, const char **error_out, int *status_out) {
    size_t i;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_sessions_lock);
    for (i = 0; i < MAX_SESSIONS; ++i) {
        if (g_sessions[i].in_use && strcmp(g_sessions[i].token, token) == 0) {
            if (g_sessions[i].expires_at < now) {
                release_session_locked(i);
                *error_out = "Token expired";
                *status_out = 403;
                pthread_mutex_unlock(&g_sessions_lock);
                return false;
            }
            if (g_sessions[i].active) {
                *error_out = "Session already active";
                *status_out = 409;
                pthread_mutex_unlock(&g_sessions_lock);
                return false;
            }
            g_sessions[i].active = true;
            g_sessions[i].started_at = now;
            *session_out = g_sessions[i];
            *idx_out = i;
            pthread_mutex_unlock(&g_sessions_lock);
            return true;
        }
    }
    pthread_mutex_unlock(&g_sessions_lock);

    *error_out = "Invalid token";
    *status_out = 403;
    return false;
}

static void release_session(size_t idx) {
    pthread_mutex_lock(&g_sessions_lock);
    release_session_locked(idx);
    pthread_mutex_unlock(&g_sessions_lock);
}

static void write_headers(FCGX_Stream *out, int status, const char *status_text, const char *content_type) {
    FCGX_FPrintF(out,
                 "Status: %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                 "Pragma: no-cache\r\n"
                 "Expires: 0\r\n\r\n",
                 status,
                 status_text,
                 content_type);
}

static void send_plain(FCGX_Stream *out, int status, const char *status_text, const char *body) {
    write_headers(out, status, status_text, "text/plain; charset=utf-8");
    if (body) {
        FCGX_FPrintF(out, "%s\n", body);
    }
}

static bool load_html_page(char **content_out, size_t *size_out) {
    const char *paths[] = {
        "./html/index.html",
        "/usr/local/packages/astralwebm/html/index.html",
        "/usr/local/packages/astralwebm/www/index.html",
    };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "rb");
        long sz;
        char *buf;
        size_t n;

        if (!f) {
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            continue;
        }
        sz = ftell(f);
        if (sz <= 0 || (unsigned long long)sz > (unsigned long long)(SIZE_MAX - 1U)) {
            fclose(f);
            continue;
        }
        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            continue;
        }
        buf = (char *)malloc((size_t)sz + 1);
        if (!buf) {
            fclose(f);
            return false;
        }
        n = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (n != (size_t)sz) {
            free(buf);
            continue;
        }
        buf[n] = '\0';
        *content_out = buf;
        *size_out = n;
        return true;
    }

    return false;
}

static void handle_index(FCGX_Request *request) {
    char *html = NULL;
    size_t html_size = 0;
    if (load_html_page(&html, &html_size)) {
        write_headers(request->out, 200, "OK", "text/html; charset=utf-8");
        (void)FCGX_PutStr(html, (int)html_size, request->out);
        free(html);
        return;
    }

    write_headers(request->out, 200, "OK", "text/html; charset=utf-8");
    (void)FCGX_PutStr(g_embedded_html, (int)strlen(g_embedded_html), request->out);
}

static void handle_session_start(FCGX_Request *request) {
    const char *query = FCGX_GetParam("QUERY_STRING", request->envp);
    const char *method = FCGX_GetParam("REQUEST_METHOD", request->envp);
    int camera = DEFAULT_CAMERA;
    int fps = DEFAULT_FPS;
    int timeout_sec = DEFAULT_TIMEOUT_SEC;
    char param_buf[32];
    char token[TOKEN_HEX_LEN + 1];

    if (method && strcmp(method, "POST") != 0 && strcmp(method, "GET") != 0) {
        send_plain(request->out, 405, "Method Not Allowed", "Use POST /session/start");
        return;
    }

    if (query_param_value(query, "camera", param_buf, sizeof(param_buf))) {
        if (!parse_positive_int(query, "camera", &camera)) {
            send_plain(request->out, 400, "Bad Request", "camera must be a positive integer");
            return;
        }
        if (camera > MAX_CAMERA) {
            send_plain(request->out, 400, "Bad Request", "camera must be between 1 and 99");
            return;
        }
    }
    if (query_param_value(query, "fps", param_buf, sizeof(param_buf))) {
        if (!parse_positive_int(query, "fps", &fps) || !is_allowed_fps(fps)) {
            send_plain(request->out, 400, "Bad Request", "fps must be one of: 1, 5, 10");
            return;
        }
    }
    if (!parse_positive_int(query, "timeout", &timeout_sec)) {
        timeout_sec = DEFAULT_TIMEOUT_SEC;
    }
    timeout_sec = clamp_timeout(timeout_sec);

    if (create_session(camera, fps, timeout_sec, token) < 0) {
        send_plain(request->out, 429, "Too Many Requests", "maximum concurrent streams reached");
        return;
    }

    write_headers(request->out, 200, "OK", "application/json; charset=utf-8");
    FCGX_FPrintF(
        request->out,
        "{\"ok\":true,\"token\":\"%s\",\"streamUrl\":\"/local/%s/stream.webm?token=%s\"}\n",
        token,
        APP_NAME,
        token);
}

static int start_transcoder_process(const stream_session_t *session, int *stdout_fd_out, pid_t *pid_out) {
    char rtsp_url[256];
    char vf_arg[64];
    char bitrate_arg[32];
    int pipefd[2];
    pid_t pid;

    snprintf(rtsp_url,
             sizeof(rtsp_url),
             "rtsp://127.0.0.1/axis-media/media.amp?camera=%d&videocodec=h264&audio=0",
             session->camera);
    snprintf(vf_arg, sizeof(vf_arg), "scale=%d:-2,fps=%d", session->width, session->fps);
    snprintf(bitrate_arg, sizeof(bitrate_arg), "%dk", session->bitrate_kbps);

    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("ffmpeg",
               "ffmpeg",
               "-loglevel",
               "error",
               "-rtsp_transport",
               "tcp",
               "-i",
               rtsp_url,
               "-an",
               "-vf",
               vf_arg,
               "-c:v",
               "libvpx",
               "-deadline",
               "realtime",
               "-cpu-used",
               "8",
               "-b:v",
               bitrate_arg,
               "-f",
               "webm",
               "-",
               (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    *stdout_fd_out = pipefd[0];
    *pid_out = pid;
    return 0;
}

static void stop_transcoder_process(pid_t pid, int stdout_fd) {
    int status = 0;
    if (stdout_fd >= 0) {
        close(stdout_fd);
    }
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
}

static void stream_from_transcoder(FCGX_Request *request, const stream_session_t *session) {
    int transcoder_fd = -1;
    pid_t transcoder_pid = -1;
    time_t start = time(NULL);
    unsigned char buffer[8192];

    if (start_transcoder_process(session, &transcoder_fd, &transcoder_pid) != 0) {
        send_plain(request->out, 502, "Bad Gateway", "Unable to start transcoder");
        return;
    }

    write_headers(request->out, 200, "OK", "video/webm");
    while (true) {
        fd_set rfds;
        struct timeval tv;
        int ready;
        ssize_t nread;

        if (difftime(time(NULL), start) >= session->timeout_sec) {
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(transcoder_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        ready = select(transcoder_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }

        nread = read(transcoder_fd, buffer, sizeof(buffer));
        if (nread > 0) {
            if (FCGX_PutStr((const char *)buffer, (int)nread, request->out) < 0 || FCGX_FFlush(request->out) < 0) {
                break;
            }
            continue;
        }
        break;
    }

    stop_transcoder_process(transcoder_pid, transcoder_fd);
}

static void handle_stream_webm(FCGX_Request *request) {
    const char *query = FCGX_GetParam("QUERY_STRING", request->envp);
    char token[TOKEN_HEX_LEN + 1];
    stream_session_t session;
    size_t slot = 0;
    const char *error = NULL;
    int status = 403;

    if (!query_param_value(query, "token", token, sizeof(token))) {
        send_plain(request->out, 403, "Forbidden", "play authorization required");
        return;
    }

    if (!activate_session_by_token(token, &session, &slot, &error, &status)) {
        send_plain(request->out, status, status == 409 ? "Conflict" : "Forbidden", error);
        return;
    }

    /* TODO: Replace ffmpeg subprocess invocation with a native transcoder path if/when dependencies are available. */
    stream_from_transcoder(request, &session);
    release_session(slot);
}

static void handle_request(FCGX_Request *request) {
    const char *path = FCGX_GetParam("PATH_INFO", request->envp);
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
        handle_index(request);
        return;
    }
    if (strcmp(path, "/session/start") == 0) {
        handle_session_start(request);
        return;
    }
    if (strcmp(path, "/stream.webm") == 0) {
        handle_stream_webm(request);
        return;
    }
    send_plain(request->out, 404, "Not Found", "Not Found");
}

int main(void) {
    const char *socket_name = getenv("FCGI_SOCKET_NAME");
    int socket_fd;
    FCGX_Request request;

    if (!socket_name || socket_name[0] == '\0') {
        socket_name = "127.0.0.1:9000";
    }

    if (FCGX_Init() != 0) {
        fprintf(stderr, "FCGX_Init failed\n");
        return 1;
    }

    socket_fd = FCGX_OpenSocket(socket_name, 16);
    if (socket_fd < 0) {
        fprintf(stderr, "FCGX_OpenSocket failed for %s\n", socket_name);
        return 1;
    }

    if (FCGX_InitRequest(&request, socket_fd, 0) != 0) {
        fprintf(stderr, "FCGX_InitRequest failed\n");
        return 1;
    }

    while (FCGX_Accept_r(&request) == 0) {
        handle_request(&request);
        FCGX_Finish_r(&request);
    }

    return 0;
}
