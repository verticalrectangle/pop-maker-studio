#include "filepicker.h"
#include <cstdio>
#include <cstring>
#include <string>

// Linux: try zenity, then kdialog, then fall back to a stdin prompt.
// macOS: use osascript.
// Windows: use Win32 COMDLG (separate impl, not needed for Linux build tonight).

std::string filepicker_open(const char* title,
                             const char* filter_name,
                             const char* filter_patterns)
{
#if defined(__APPLE__)
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file with prompt \"%s\")' 2>/dev/null",
        title);
    FILE* p = popen(cmd, "r");
    if (!p) return "";
    char buf[4096] = {};
    fgets(buf, sizeof(buf), p);
    pclose(p);
    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;

#elif defined(_WIN32)
    // TODO: Win32 COMDLG implementation
    (void)title; (void)filter_name; (void)filter_patterns;
    return "";

#else
    // Linux — try zenity first, then kdialog
    char cmd[2048];

    // Build zenity filter string: "Audio & Video | *.wav *.mp3 *.mp4"
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --title='%s' "
        "--file-filter='%s | %s' 2>/dev/null",
        title, filter_name, filter_patterns);
    FILE* p = popen(cmd, "r");
    if (p) {
        char buf[4096] = {};
        fgets(buf, sizeof(buf), p);
        int ret = pclose(p);
        if (ret == 0 && buf[0] != '\0') {
            std::string result(buf);
            while (!result.empty() &&
                   (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (!result.empty()) return result;
        }
    }

    // Fallback: kdialog
    snprintf(cmd, sizeof(cmd),
        "kdialog --getopenfilename . '%s' --title '%s' 2>/dev/null",
        filter_patterns, title);
    p = popen(cmd, "r");
    if (p) {
        char buf[4096] = {};
        fgets(buf, sizeof(buf), p);
        int ret = pclose(p);
        if (ret == 0 && buf[0] != '\0') {
            std::string result(buf);
            while (!result.empty() &&
                   (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (!result.empty()) return result;
        }
    }

    return "";
#endif
}

std::string filepicker_save(const char* title,
                             const char* filter_name,
                             const char* filter_patterns,
                             const char* default_path)
{
#if defined(__APPLE__)
    (void)default_path;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file name with prompt \"%s\")' 2>/dev/null",
        title);
    FILE* p = popen(cmd, "r");
    if (!p) return "";
    char buf[4096] = {};
    fgets(buf, sizeof(buf), p);
    pclose(p);
    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;

#elif defined(_WIN32)
    (void)title; (void)filter_name; (void)filter_patterns;
    return "";

#else
    char cmd[2048];
    // Seed the starting dir + suggested name when a default is given.
    char fnarg[1280] = {};
    if (default_path && *default_path)
        snprintf(fnarg, sizeof(fnarg), "--filename='%s' ", default_path);
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --save --confirm-overwrite --title='%s' "
        "%s--file-filter='%s | %s' 2>/dev/null",
        title, fnarg, filter_name, filter_patterns);
    FILE* p = popen(cmd, "r");
    if (p) {
        char buf[4096] = {};
        fgets(buf, sizeof(buf), p);
        int ret = pclose(p);
        if (ret == 0 && buf[0] != '\0') {
            std::string result(buf);
            while (!result.empty() &&
                   (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (!result.empty()) return result;
        }
    }

    snprintf(cmd, sizeof(cmd),
        "kdialog --getsavefilename '%s' '%s' --title '%s' 2>/dev/null",
        (default_path && *default_path) ? default_path : ".", filter_patterns, title);
    p = popen(cmd, "r");
    if (p) {
        char buf[4096] = {};
        fgets(buf, sizeof(buf), p);
        int ret = pclose(p);
        if (ret == 0 && buf[0] != '\0') {
            std::string result(buf);
            while (!result.empty() &&
                   (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (!result.empty()) return result;
        }
    }

    return "";
#endif
}
