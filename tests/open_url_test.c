/* Exercise the real Linux browser handoff without opening a real browser. */
#include "../src/auth/rnet_open_url.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static void write_opener(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs("#!/bin/sh\n"
          "printf '%s' \"$1\" > \"$RNET_TEST_URL_FILE\"\n"
          "printf '%s' \"$0\" > \"$RNET_TEST_OPENER_FILE\"\n"
          "/usr/bin/env > \"$RNET_TEST_ENV_FILE\"\n"
          ": > \"$RNET_TEST_DONE_FILE\"\n", f);
    fclose(f);
    if (chmod(path, 0755) != 0) { perror("chmod"); exit(2); }
}

static int read_file(const char *path, char *out, size_t cap) {
    int i;
    for (i = 0; i < 100; ++i) {
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(out, 1, cap - 1, f);
            fclose(f);
            out[n] = '\0';
            return 1;
        }
        usleep(10000);
    }
    out[0] = '\0';
    return 0;
}

int main(void) {
    char dir[] = "/tmp/rnet-open-url-XXXXXX";
    char xdg[256], wsl[256], url_file[256], env_file[256], opener_file[256], done_file[256], out[8192];
    const char *url = "https://discord.example/authorize?client_id=1&scope=identify";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(xdg, sizeof(xdg), "%s/xdg-open", dir);
    snprintf(wsl, sizeof(wsl), "%s/wslview", dir);
    snprintf(url_file, sizeof(url_file), "%s/url", dir);
    snprintf(env_file, sizeof(env_file), "%s/env", dir);
    snprintf(opener_file, sizeof(opener_file), "%s/opener", dir);
    snprintf(done_file, sizeof(done_file), "%s/done", dir);
    setenv("PATH", dir, 1);
    setenv("RNET_TEST_URL_FILE", url_file, 1);
    setenv("RNET_TEST_ENV_FILE", env_file, 1);
    setenv("RNET_TEST_OPENER_FILE", opener_file, 1);
    setenv("RNET_TEST_DONE_FILE", done_file, 1);
    setenv("APPIMAGE", "/tmp/Game.AppImage", 1);
    setenv("APPDIR", "/tmp/appdir", 1);
    setenv("LD_LIBRARY_PATH", "/tmp/appdir/usr/lib", 1);
    setenv("GTK_PATH", "/tmp/appdir/gtk", 1);
    setenv("RECOMP_HOST_LD_LIBRARY_PATH", "/opt/host/lib", 1);
    setenv("DISPLAY", ":0", 1);

    write_opener(xdg);
    check(rnet_open_url(url), "xdg-open starts");
    check(read_file(done_file, out, sizeof(out)), "xdg-open stub finished");
    check(read_file(url_file, out, sizeof(out)) && strcmp(out, url) == 0,
          "OAuth URL is passed as one unchanged argument");
    check(read_file(env_file, out, sizeof(out)), "opener recorded environment");
    check(strstr(out, "LD_LIBRARY_PATH=/opt/host/lib\n") != NULL,
          "host library path is restored");
    check(strstr(out, "APPIMAGE=") == NULL && strstr(out, "APPDIR=") == NULL &&
          strstr(out, "GTK_PATH=") == NULL,
          "AppImage variables are removed from opener environment");
    check(strstr(out, "DISPLAY=:0\n") != NULL,
          "desktop variables reach opener");

    write_opener(wsl);
    setenv("WSL_INTEROP", "/run/WSL/test_interop", 1);
    unlink(url_file);
    unlink(env_file);
    unlink(opener_file);
    unlink(done_file);
    check(rnet_open_url(url), "WSL browser bridge starts");
    check(read_file(done_file, out, sizeof(out)), "wslview stub finished");
    check(read_file(opener_file, out, sizeof(out)) && strcmp(out, wsl) == 0,
          "wslview is preferred while xdg-open also exists");
    unsetenv("WSL_INTEROP");

    unlink(xdg);
    unlink(url_file);
    unlink(env_file);
    unlink(opener_file);
    unlink(done_file);
    setenv("RECOMP_HOST_LD_LIBRARY_PATH", "", 1);
    check(rnet_open_url(url), "wslview is used when xdg-open is absent");
    check(read_file(done_file, out, sizeof(out)), "wslview fallback finished");
    check(read_file(url_file, out, sizeof(out)) && strcmp(out, url) == 0,
          "wslview receives the OAuth URL");
    check(read_file(env_file, out, sizeof(out)), "wslview recorded environment");
    check(strstr(out, "LD_LIBRARY_PATH=") == NULL,
          "AppImage library path is absent without a saved host path");

    unlink(wsl);
    check(!rnet_open_url(url), "missing opener is reported");
    check(!rnet_open_url("file:///tmp/not-a-url"), "non-HTTP URL is refused");
    unlink(url_file);
    unlink(env_file);
    unlink(opener_file);
    unlink(done_file);
    rmdir(dir);
    printf("open_url_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
