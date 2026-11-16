// The NickelHook runtime functions that src/nickelsync.cc calls. On the device
// they are in NickelHook/nh.c, which also patches Nickel, so the sim does not
// build that file.
#include <QDir>
#include <QFile>

#include <cstdarg>
#include <cstdio>

#include <NickelHook.h>

void nh_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("nh: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

void nh_dump_log() {}

bool nh_delete_file(const char *path) { return !QFile::exists(path) || QFile::remove(path); }

bool nh_delete_dir(const char *path) { return !QDir(path).exists() || QDir().rmdir(path); }
