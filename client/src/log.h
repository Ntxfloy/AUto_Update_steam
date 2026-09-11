// log.h - verbose client logger: rotating file + background upload to the VPS
//
// Goals:
//   * every meaningful step is recorded with a timestamp (ms), thread id,
//     module tag, current app id and session id;
//   * the raw SteamCMD output is kept verbatim in the file (the GUI keeps
//     showing only the filtered lines);
//   * lines are mirrored to the server so you can debug 50 machines from the
//     admin panel instead of walking around with a flash drive;
//   * logging never blocks the worker: uploads happen on their own thread and
//     a dead server only costs memory, not speed.
#pragma once

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
} LogLevel;

// Open (or create) the log file and start the uploader thread.
// file_path: full path, e.g. "C:\\updater\\updater.log".
// The file is rotated at ~5 MB, keeping updater.log.1 .. updater.log.3.
void log_init(const char *file_path, LogLevel min_level);

// Identify this machine. Call as soon as the config is loaded.
void log_set_pc_id(const char *pc_id);

// Current game being processed; appears in every following line. NULL clears.
void log_set_app(const char *app_id);

// Enable uploading to the server. Safe to call before or after log_init.
// enabled = 0 turns the upload off (the file keeps working).
void log_set_remote(const char *server_url, const char *api_key, int enabled);

// Minimum level that gets uploaded (the file always gets everything above
// the log_init level). Defaults to LOG_LEVEL_DEBUG so that a test run is
// fully reproducible from the admin panel.
void log_set_remote_level(LogLevel level);

// Random id generated at startup - one "run" of the updater.
const char *log_session_id(void);

// Path of the active log file (for the "Open log" button).
const char *log_file_path(void);

// Core entry point. module is a short tag: "worker", "steamcmd", "ui", "api".
void log_write(LogLevel level, const char *module, const char *fmt, ...);

// Verbatim line (raw SteamCMD stdout). Always DEBUG level, never reformatted.
void log_raw(const char *module, const char *line);

// Push whatever is queued to the server right now (called at the end of each
// game and on exit). Waits up to timeout_ms.
void log_flush_remote(int timeout_ms);

// Flush + stop the uploader thread + close the file.
void log_shutdown(void);

// How many lines are waiting to be uploaded (0 = server is keeping up).
int  log_pending_count(void);

#define LOG_DEBUG(mod, ...) log_write(LOG_LEVEL_DEBUG, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...)  log_write(LOG_LEVEL_INFO,  mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  log_write(LOG_LEVEL_WARN,  mod, __VA_ARGS__)
#define LOG_ERROR(mod, ...) log_write(LOG_LEVEL_ERROR, mod, __VA_ARGS__)
