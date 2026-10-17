// UM98x firmware upgrade from Linux, over a serial port (USB-UART on COM1).
//
// Port of decipede's um980_fw.c bootloader logic:
//   gcc -O2 -Wall -o um98x_flash um98x_flash.c
//   ./um98x_flash [-d /dev/ttyUSB0] [-b 115200] [-f 921600] [-r cmd|rts|dtr|none] [-V version] [-y] [-v] [-j] firmware.pkg
//
// -b  current COM1 baud rate (also used for the transfer if -f fails or is 0)
// -f  fast transfer rate, 0 to disable
// -r  how to restart the receiver into its bootloader:
//       cmd  send "RESET" (default); rts/dtr  pulse that line (if wired to RESET);
//       none just wait for a manual power cycle (rescue of a silent receiver)
// -V  package version (e.g. R4.10Build20739): skip if the receiver already runs this
//     build or newer, and require exactly this build after flashing. Default: taken
//     from a Unicore-style file name (UM980_R4.10Build20739.pkg), if any.
// -y  force: flash even on a model mismatch, or a build not newer than the receiver's
// -v  echo what the receiver sends (stderr)
// -j  machine output: one JSON object per line on stdout, nothing else there
//       {"event":"package","product":"UM980","size":3038408}
//       {"event":"receiver","model":"UM980","firmware":"R4.10Build11833"}
//       {"event":"stage","stage":"bootloader","msg":"..."}   stages: baud, bootloader,
//                                                           transfer, verify, reboot, check
//       {"event":"progress","percent":42,"sent":1276928,"total":3038408}
//       {"event":"action","msg":"Power-cycle the receiver now"}   (needs the user)
//       {"event":"log","msg":"..."}  {"event":"error","msg":"..."}
//       {"event":"result","ok":true,"skipped":false,"firmware":"R4.10Build20739","previous":"...","error":null}
//     "skipped": true when -V says the receiver is already up to date (nothing flashed).
//     "result" is always the last line, except for usage/argument errors (exit 1, stderr).
//     Exit code: 0 ok, 1 failed (receiver fine), 2 receiver not answering.
//
// The bootloader only talks on COM1: use the port wired to COM1.
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ACK_TIMEOUT_MS 30000u
#define PKT_DATA       1024u
#define PKT_SIZE       (3u + PKT_DATA + 1u)
#define PKG_HEADER_SIZE 0xC0
#define XM_STX 0x02
#define XM_EOT 0x04
#define XM_ACK 0x06
#define XM_NAK 0x15
#define XM_CAN 0x18

static int fd = -1;
static bool verbose;
static bool json;
static bool skipped;
static const char *reset_mode = "cmd";
static uint8_t *pkg;
static uint32_t pkg_size;
static const char *boot_error;

// --- Output --------------------------------------------------------------------------

static void jstr(const char *s) {
    if (!s) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

// Human text (errors to stderr), or {"event":<event>,["stage":<stage>,]"msg":...}.
static void vsay(const char *event, const char *stage, const char *fmt, va_list ap) {
    char text[256];
    vsnprintf(text, sizeof(text), fmt, ap);
    if (!json) {
        if (!strcmp(event, "error")) fprintf(stderr, "✗ %s\n", text);
        else printf("%s%s\n", stage ? "== " : "", text);
        return;
    }
    printf("{\"event\":\"%s\",", event);
    if (stage) printf("\"stage\":\"%s\",", stage);
    fputs("\"msg\":", stdout);
    jstr(text);
    fputs("}\n", stdout);
}

static void say(const char *event, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(event, NULL, fmt, ap);
    va_end(ap);
}

static void stage(const char *name, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay("stage", name, fmt, ap);
    va_end(ap);
}

static void progress(uint32_t sent, uint32_t total) {
    int pct = (int)((uint64_t)sent * 100 / total);
    if (json) printf("{\"event\":\"progress\",\"percent\":%d,\"sent\":%u,\"total\":%u}\n", pct, sent, total);
    else printf("\r%3d%%%s", pct, sent == total ? "\n" : "");
    fflush(stdout);
}

// Final line; returns the exit code.
static int result(bool ok, const char *fw, const char *prev, const char *error, int code) {
    if (json) {
        printf("{\"event\":\"result\",\"ok\":%s,\"skipped\":%s,\"firmware\":", ok ? "true" : "false",
               skipped ? "true" : "false");
        jstr(fw && *fw ? fw : NULL);
        fputs(",\"previous\":", stdout);
        jstr(prev && *prev ? prev : NULL);
        fputs(",\"error\":", stdout);
        jstr(error);
        fputs("}\n", stdout);
    } else if (ok && skipped) {
        printf("✓ receiver already up to date (%s)\n", fw);
    } else if (ok) {
        printf("✓ receiver now runs %s (was %s)\n", fw, prev && *prev ? prev : "unknown");
    }
    fflush(stdout);
    return code;
}

// --- Serial port ---------------------------------------------------------------------

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(uint32_t ms) {
    usleep(ms * 1000u);
}

static speed_t baud_const(uint32_t baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

static bool set_baud(uint32_t baud) {
    struct termios t;
    speed_t s = baud_const(baud);
    if (!s || tcgetattr(fd, &t) != 0) return false;
    tcdrain(fd);
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | CSTOPB);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, s);
    cfsetospeed(&t, s);
    return tcsetattr(fd, TCSANOW, &t) == 0;
}

static void port_write(const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                struct pollfd pfd = {fd, POLLOUT, 0};
                poll(&pfd, 1, 100);
                continue;
            }
            say("error", "write: %s", strerror(errno));
            exit(result(false, NULL, NULL, strerror(errno), 2));
        }
        p += w;
        len -= (size_t)w;
    }
    tcdrain(fd);
}

static void port_puts(const char *s) {
    port_write(s, strlen(s));
}

static void discard_input(void) {
    uint8_t buf[256];
    tcflush(fd, TCIFLUSH);
    while (read(fd, buf, sizeof(buf)) > 0) {
    }
}

static int read_byte(uint32_t timeout_ms) {
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, (int)timeout_ms) <= 0) return -1;
    uint8_t c;
    if (read(fd, &c, 1) != 1) return -1;
    if (verbose) fputc(isprint(c) || c == '\n' || c == '\r' ? c : '.', stderr);
    return c;
}

static bool wait_string(const char *needle, uint32_t timeout_ms) {
    char win[64];
    size_t n = strlen(needle), have = 0;
    uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        int c = read_byte(50);
        if (c < 0) continue;
        if (have == n) {
            memmove(win, win + 1, n - 1);
            have--;
        }
        win[have++] = (char)c;
        if (have == n && memcmp(win, needle, n) == 0) return true;
    }
    return false;
}

static int wait_byte(const uint8_t *accept, size_t n, uint32_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        int c = read_byte(50);
        if (c < 0) continue;
        for (size_t i = 0; i < n; i++)
            if (c == accept[i]) return c;
    }
    return -1;
}

// --- Receiver ------------------------------------------------------------------------

// VERSIONA -> model ("UM980") and firmware ("R4.10Build11833").
static bool query_version(char *model, size_t msize, char *fw, size_t fsize, uint32_t timeout_ms) {
    char line[512];
    size_t len = 0;
    discard_input();
    port_puts("VERSIONA\r\n");
    uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        int c = read_byte(50);
        if (c < 0) continue;
        if (c != '\n') {
            if (len < sizeof(line) - 1) line[len++] = (char)c;
            continue;
        }
        line[len] = '\0';
        len = 0;
        // #VERSIONA,...;"UM980","R4.10Build11833",...
        if (!strstr(line, "#VERSIONA")) continue;
        char *f = strrchr(line, ';');
        f = f ? f + 1 : line;
        char m[32] = "", v[64] = "";
        if (sscanf(f, "\"%31[^\"]\",\"%63[^\"]\"", m, v) < 1 && sscanf(f, "%31[^,],%63[^,]", m, v) < 1) continue;
        if (model) snprintf(model, msize, "%s", m);
        if (fw) snprintf(fw, fsize, "%s", v);
        return m[0] != '\0';
    }
    return false;
}

// Switch COM1 and our port to `baud`; returns true if the receiver answers.
static bool switch_baud(uint32_t baud) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "CONFIG COM1 %u\r\n", baud);
    discard_input();
    port_puts(cmd);
    sleep_ms(200);
    set_baud(baud);
    sleep_ms(100);
    return query_version(NULL, 0, NULL, 0, 1500);
}

static bool fail(const char *e) {
    boot_error = e;
    say("error", "%s", e);
    return false;
}

static void reset_receiver(void) {
    if (!strcmp(reset_mode, "cmd")) {
        port_puts("RESET\r\n");
    } else if (!strcmp(reset_mode, "rts") || !strcmp(reset_mode, "dtr")) {
        int bit = reset_mode[0] == 'r' ? TIOCM_RTS : TIOCM_DTR;
        ioctl(fd, TIOCMBIS, &bit);
        sleep_ms(500);
        ioctl(fd, TIOCMBIC, &bit);
    }
}

static bool enter_bootloader(void) {
    discard_input();
    reset_receiver();
    // Spam the trigger until the bootloader menu shows up.
    static const char trigger[] = "T@T@T@T@T@T@T@T@";
    bool manual = !strcmp(reset_mode, "none");
    if (manual) say("action", "Power-cycle the receiver now (60 s)");
    uint64_t start = now_ms(), deadline = start + (manual ? 60000 : 10000);
    char win[16];
    const char *needle = "efuse from uart";
    size_t n = strlen(needle), have = 0;
    uint64_t next_send = 0;
    while (now_ms() < deadline) {
        if (now_ms() >= next_send) {
            port_write(trigger, sizeof(trigger) - 1);
            next_send = now_ms() + 20;
        }
        int c = read_byte(2);
        if (c < 0) continue;
        if (have == n) {
            memmove(win, win + 1, n - 1);
            have--;
        }
        win[have++] = (char)c;
        if (have == n && memcmp(win, needle, n) == 0) return true;
    }
    return fail("bootloader menu not reached (try -r none and power-cycle)");
}

static bool send_packets(void) {
    static uint8_t pkt[PKT_SIZE];
    static const uint8_t replies[] = {XM_ACK, XM_NAK, XM_CAN};
    uint8_t seq = 1;
    uint32_t sent = 0, packets = 0;
    int last_pct = -1;
    while (sent < pkg_size) {
        uint32_t n = pkg_size - sent < PKT_DATA ? pkg_size - sent : PKT_DATA;
        pkt[0] = XM_STX;
        pkt[1] = seq;
        pkt[2] = (uint8_t)(0xFF - seq);
        memcpy(pkt + 3, pkg + sent, n);
        if (n < PKT_DATA) {
            pkt[3 + n] = 0x1A;  // EOF marker, then zero padding
            memset(pkt + 3 + n + 1, 0, PKT_DATA - n - 1);
        }
        uint8_t sum = 0;
        for (unsigned i = 0; i < PKT_SIZE - 1; i++) sum += pkt[i];
        pkt[PKT_SIZE - 1] = (uint8_t)(sum - 1);

        int naks = 0, timeouts = 0;
        for (;;) {
            discard_input();  // only a reply to this packet counts
            port_write(pkt, PKT_SIZE);
            uint64_t t_sent = now_ms();
            // The receiver can pause for seconds (flash erase): wait patiently
            // rather than resending into a busy receiver.
            int r = wait_byte(replies, sizeof(replies), ACK_TIMEOUT_MS);
            uint64_t ms = now_ms() - t_sent;
            if (ms > 2000) say("log", "packet %u reply after %llu ms", packets, (unsigned long long)ms);
            if (r == XM_ACK) break;
            if (r == XM_CAN) return fail("transfer cancelled by receiver");
            if (r < 0 && ++timeouts > 1) return fail("no ACK from receiver");
            if (r == XM_NAK && ++naks > 3) return fail("packet rejected 3 times");
            say("log", "packet %u %s, resending", packets, r < 0 ? "timeout" : "NAK");
        }
        sent += n;
        packets++;
        seq++;
        int pct = (int)((uint64_t)sent * 100 / pkg_size);
        if (pct != last_pct) {
            last_pct = pct;
            progress(sent, pkg_size);
        }
    }
    say("log", "%u packets sent", packets);
    return true;
}

// One complete bootloader session. Leaves our port at `normal`.
static bool bootload(uint32_t baud, uint32_t normal) {
    boot_error = NULL;
    stage("bootloader", "entering bootloader at %u baud", baud);
    bool ok = enter_bootloader();
    if (ok) {
        stage("transfer", "bootloader menu reached, sending %u bytes", pkg_size);
        port_write("\r\n2\r\n", 5);
        if (!wait_string("download to", 10000)) ok = fail("download mode not entered");
    }
    if (ok) {
        static const uint8_t nak = XM_NAK;
        if (wait_byte(&nak, 1, 10000) < 0) ok = fail("receiver not ready (no 0x15)");
        sleep_ms(50);
        discard_input();  // drop further "ready" polls
    }
    if (ok) ok = send_packets();
    if (ok) {
        static const uint8_t eot = XM_EOT;
        discard_input();
        port_write(&eot, 1);
        stage("verify", "transfer complete, receiver verifying (up to 90 s)");
        if (!wait_string("backup succeed", 90000)) ok = fail("no 'backup succeed'");
    }
    // Leave the bootloader in every case.
    stage("reboot", "leaving bootloader");
    if (!wait_string("efuse from uart", ok ? 10000 : 1000) && ok) say("log", "menu not reprinted, exiting anyway");
    port_write("6\r\n6\r\n6\r\n", 9);
    bool reset_seen = wait_string("resetting the cpu", 10000);
    set_baud(normal);
    if (!reset_seen) reset_receiver();  // make sure it restarts
    wait_string("$devicename,COM", 8000);
    sleep_ms(1000);
    return ok;
}

// --- Main ----------------------------------------------------------------------------

static long build_number(const char *version) {
    const char *p = version ? strstr(version, "Build") : NULL;
    if (!p || !isdigit((unsigned char)p[5])) return -1;
    return strtol(p + 5, NULL, 10);
}

static bool load_pkg(const char *path, char *product, size_t psize) {
    static const uint8_t magic[4] = {0xa5, 0xa4, 0xa3, 0xa2};
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0 || st.st_size < PKG_HEADER_SIZE) {
        say("error", "cannot read %s", path);
        return false;
    }
    pkg_size = (uint32_t)st.st_size;
    pkg = malloc(pkg_size);
    bool ok = pkg && fread(pkg, 1, pkg_size, f) == pkg_size;
    fclose(f);
    if (!ok || memcmp(pkg, magic, 4) != 0) {
        say("error", "%s: not a Unicore .pkg", path);
        return false;
    }
    snprintf(product, psize, "%.*s", (int)(PKG_HEADER_SIZE - 0x9C), (const char *)pkg + 0x9C);
    return true;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/ttyUSB0";
    uint32_t normal = 115200, fast = 921600;
    const char *target = NULL;
    bool yes = false;
    int opt;
    while ((opt = getopt(argc, argv, "d:b:f:r:V:yvj")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'b': normal = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'f': fast = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'r': reset_mode = optarg; break;
        case 'V': target = optarg; break;
        case 'y': yes = true; break;
        case 'v': verbose = true; break;
        case 'j': json = true; break;
        default: goto usage;
        }
    }
    if (optind != argc - 1 || !baud_const(normal) || (fast && !baud_const(fast)) ||
        (strcmp(reset_mode, "cmd") && strcmp(reset_mode, "rts") && strcmp(reset_mode, "dtr") && strcmp(reset_mode, "none")) ||
        (target && build_number(target) < 0))
        goto usage;
    if (json) setvbuf(stdout, NULL, _IOLBF, 0);  // one event per line, flushed for the pipe reader
    // No -V: take the version from a Unicore-style file name (UM980_R4.10Build20739.pkg).
    char name_version[64];
    if (!target) {
        const char *b = strrchr(argv[optind], '/');
        b = b ? b + 1 : argv[optind];
        const char *s = strstr(b, "Build");
        while (s && s > b && s[-1] != '_') s--;
        if (s && build_number(s) >= 0) {
            const char *e = strstr(s, ".pkg");
            snprintf(name_version, sizeof(name_version), "%.*s", (int)(e ? e - s : (long)strlen(s)), s);
            target = name_version;
            say("log", "version %s taken from the file name", target);
        }
    }
    long want = target ? build_number(target) : -1;

    char product[40];
    if (!load_pkg(argv[optind], product, sizeof(product))) return result(false, NULL, NULL, "invalid package", 1);
    if (json) {
        fputs("{\"event\":\"package\",\"product\":", stdout);
        jstr(product);
        printf(",\"size\":%u}\n", pkg_size);
    } else {
        printf("package: %s, %u bytes\n", product, pkg_size);
    }

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0 || !set_baud(normal)) {
        say("error", "%s: %s", dev, strerror(errno));
        return result(false, NULL, NULL, "cannot open serial port", 1);
    }

    char model[32] = "", fw_before[64] = "", fw_after[64] = "";
    bool up = query_version(model, sizeof(model), fw_before, sizeof(fw_before), 3000);
    if (up) {
        if (json) {
            fputs("{\"event\":\"receiver\",\"model\":", stdout);
            jstr(model);
            fputs(",\"firmware\":", stdout);
            jstr(fw_before);
            fputs("}\n", stdout);
        } else {
            printf("receiver: %s %s\n", model, fw_before);
        }
        if (strcmp(model, product) != 0 && !yes) {
            say("error", "package is for %s, receiver is %s (use -y to force)", product, model);
            return result(false, fw_before, NULL, "model mismatch", 1);
        }
        long have = build_number(fw_before);
        if (want >= 0 && have >= want && !yes) {
            skipped = true;
            return result(true, fw_before, fw_before, NULL, 0);
        }
    } else {
        say("log", "receiver silent at %u baud: rescue mode", normal);
        if (!strcmp(reset_mode, "cmd")) reset_mode = "none";
        fast = 0;
    }

    // Fast transfer if the receiver accepts it, else the normal rate.
    if (fast) stage("baud", "switching to %u baud", fast);
    uint32_t baud = fast && switch_baud(fast) ? fast : normal;
    if (fast && baud != fast) switch_baud(normal);
    bool ok = bootload(baud, normal);
    if (!ok && baud != normal) {
        say("log", "retrying at %u baud", normal);
        ok = bootload(normal, normal);
    }

    stage("check", "waiting for the receiver to answer");
    for (int i = 0; i < 3 && !query_version(NULL, 0, fw_after, sizeof(fw_after), 3000); i++) sleep_ms(10000);
    if (!fw_after[0]) {
        say("error", "receiver not answering after flashing (%s)", boot_error ? boot_error : "no answer");
        return result(false, NULL, fw_before, boot_error ? boot_error : "receiver not answering", 2);
    }
    if (!ok) {
        say("error", "upgrade failed (%s), receiver on %s", boot_error, fw_after);
        return result(false, fw_after, fw_before, boot_error, 1);
    }
    if (want >= 0 && build_number(fw_after) != want) {
        say("error", "receiver runs %s, expected %s", fw_after, target);
        return result(false, fw_after, fw_before, "version not confirmed", 1);
    }
    if (want < 0 && build_number(fw_after) >= 0 && build_number(fw_after) == build_number(fw_before))
        say("log", "build number unchanged");
    return result(true, fw_after, fw_before, NULL, 0);

usage:
    fprintf(stderr, "usage: %s [-d dev] [-b baud] [-f fastbaud|0] [-r cmd|rts|dtr|none] [-V version] [-y] [-v] [-j] firmware.pkg\n", argv[0]);
    return 1;
}
