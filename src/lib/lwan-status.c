/*
 * lwan - web server
 * Copyright (c) 2021 L. A. F. Pereira <l@tia.mat.br>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "lwan-private.h"
#include "lwan-status.h"

enum lwan_status_type {
    STATUS_INFO = 0,
    STATUS_WARNING = 1,
    STATUS_ERROR = 2,
    STATUS_DEBUG = 3,
    STATUS_PERROR = 4,
    STATUS_NONE = 5,
    /* [6,7] are unused so that CRITICAL can be ORed with previous items */
    STATUS_CRITICAL = 8,
};

static bool can_use_colors(void);
static bool can_use_emojis(void);

static volatile bool quiet = false;
static bool use_colors;
static bool use_emojis;

void lwan_status_init(struct lwan *l)
{
#ifdef NDEBUG
    quiet = l->config.quiet;
#else
    quiet = false;
    (void)l;
#endif
    use_colors = can_use_colors();
    use_emojis = can_use_emojis();
}

void lwan_status_shutdown(struct lwan *l __attribute__((unused))) {}

static bool can_use_colors(void)
{
    const char *term;
    const char *no_color;

    if (!isatty(fileno(stdout)))
        return false;

    /* From https://no-color.org: "Command-line software which adds ANSI
     * color to its output by default should check for a NO_COLOR
     * environment variable that, when present and not an empty string
     * (regardless of its value), prevents the addition of ANSI color."  */
    no_color = secure_getenv("NO_COLOR");
    if (no_color && no_color[0])
        return false;

    term = secure_getenv("TERM");
    if (term && streq(term, "dumb"))
        return false;

    return true;
}

static bool can_use_emojis(void)
{
    if (!can_use_colors())
        return false;

    const char *lang = secure_getenv("LANG");
    if (!lang)
        return false;

    if (!strstr(lang, ".UTF-8"))
        return false;

    return true;
}

static int status_index(enum lwan_status_type type)
{
    return use_colors ? (int)type : STATUS_NONE;
}

#define V(c) { .value = c, .len = sizeof(c) - 1 }
static const struct lwan_value start_colors[] = {
    [STATUS_INFO] = V("\033[36m"),
    [STATUS_WARNING] = V("\033[33m"),
    [STATUS_DEBUG] = V("\033[37m"),
    [STATUS_PERROR] = V("\033[35m"),
    [STATUS_CRITICAL] = V("\033[31;1m"),
    [STATUS_NONE] = V(""),
    [STATUS_ERROR] = V("\033[35m"),
    [STATUS_CRITICAL | STATUS_PERROR] = V("\033[31;1m"),
};

static inline struct lwan_value start_color(enum lwan_status_type type)
{
    return start_colors[status_index(type)];
}

static inline struct lwan_value end_color(void)
{
    return use_colors ? (struct lwan_value)V("\033[0m\n")
                      : (struct lwan_value)V("\n");
}
#undef V

static inline char *strerror_thunk_r(int error_number, char *buffer, size_t len)
{
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return strerror_r(error_number, buffer, len);
#else /* XSI-compliant strerror_r() */
    if (!strerror_r(error_number, buffer, len))
        return buffer;
    return "Unknown";
#endif
}

#ifndef NDEBUG

LWAN_LAZY_THREAD_LOCAL(long, gettid_cached)
{
    return gettid();
}

LWAN_LAZY_THREAD_LOCAL(const char *, get_thread_emoji)
{
    static const char *emojis[] = {
        "🐶", "🐱", "🐭", "🐹", "🐰", "🦊", "🐻", "🐼", "🐨", "🐯", "🦁", "🐮",
        "🐷", "🐽", "🐸", "🐵", "🐔", "🐧", "🐦", "🐤", "🦆", "🦉", "🦇", "🐺",
        "🐗", "🐴", "🦄", "🐝", "🪱", "🐛", "🦋", "🐌", "🐞", "🐜", "🪰", "🪲",
        "🪳", "🦟", "🦗", "🦂", "🐢", "🐍", "🦎", "🦖", "🦕", "🐙", "🦑", "🦐",
        "🦞", "🦀", "🐡", "🐠", "🐟", "🐬", "🐳", "🐋", "🦈", "🦭", "🐊", "🐅",
        "🐆", "🦓", "🦍", "🦧", "🦣", "🐘", "🦛", "🦏", "🐪", "🐫", "🦒", "🦘",
        "🦬", "🐃", "🐂", "🐄", "🐎", "🐖", "🐏", "🐑", "🦙", "🐐", "🦌", "🐕",
        "🐩", "🐈", "🐓", "🦃", "🦤", "🦚", "🦜", "🦢", "🦩", "🕊", "🐇", "🦝",
        "🦨", "🦡", "🦫", "🦦", "🦥", "🐁", "🐀", "🐿", "🦔", "🐉", "🐲",
    };
    static unsigned int last_emoji_id;
    return emojis[ATOMIC_INC(last_emoji_id) % (int)N_ELEMENTS(emojis)];
}
#endif

#define FORMAT_WITH_COLOR(fmt, color) "\033[" color "m" fmt "\033[0m"

#ifdef LWAN_HAVE_SYSLOG

#include <lwan-strbuf.h>
#include <syslog.h>

static int status_to_syslog_prio[] = {
    [STATUS_CRITICAL | STATUS_PERROR] = LOG_CRIT,
    [STATUS_CRITICAL] = LOG_CRIT,
    [STATUS_ERROR] = LOG_ERR,
    [STATUS_WARNING] = LOG_WARNING,
    [STATUS_INFO] = LOG_INFO,
    [STATUS_DEBUG] = LOG_DEBUG,
};

void lwan_syslog_status_out(
#ifndef NDEBUG
    const char *file,
    const int line,
    const char *func,
    const long tid,
#endif
    enum lwan_status_type type,
    int saved_errno,
    const char *fmt,
    va_list values)
{
    char syslog_buffer[256];
    va_list copied_values;
    struct lwan_strbuf buf;

    lwan_strbuf_init_with_fixed_buffer(&buf, syslog_buffer,
                                       sizeof(syslog_buffer));

#ifndef NDEBUG
    if (!lwan_strbuf_append_printf(&buf, "%ld %s:%d %s() ", tid,
                                   basename(strdupa(file)), line, func))
        goto out;
#endif

    va_copy(copied_values, values);
    if (!lwan_strbuf_append_vprintf(&buf, fmt, copied_values))
        goto out;

    if (type & STATUS_PERROR) {
        char errbuf[128];

        if (!lwan_strbuf_append_strz(
                &buf,
                strerror_thunk_r(saved_errno, errbuf, sizeof(errbuf) - 1)))
            goto out;
    }

    syslog(status_to_syslog_prio[type], "%.*s",
           (int)lwan_strbuf_get_length(&buf), lwan_strbuf_get_buffer(&buf));

out:
    lwan_strbuf_free(&buf);
}

LWAN_CONSTRUCTOR(register_lwan_to_syslog, 0)
{
    openlog("lwan", LOG_NDELAY | LOG_PID | LOG_CONS, LOG_USER);
}
#else
#define lwan_syslog_status_out(...)
#endif

static void status_out(
#ifndef NDEBUG
    const char *file,
    const int line,
    const char *func,
#endif
    enum lwan_status_type type,
    const char *fmt,
    va_list values)
{
    struct lwan_value start = start_color(type);
    struct lwan_value end = end_color();
    int saved_errno = errno;

#ifndef NDEBUG
    lwan_syslog_status_out(file, line, func, gettid_cached(), type, saved_errno,
                           fmt, values);
#else
    lwan_syslog_status_out(type, saved_errno, fmt, values);
#endif

    flockfile(stdout);

#ifndef NDEBUG
    char *base_name = basename(strdupa(file));
    if (LIKELY(use_colors)) {
        if (LIKELY(use_emojis))
            printf("%s ", get_thread_emoji());
        else
            printf(FORMAT_WITH_COLOR("%ld ", "32;1"), gettid_cached());
        printf(FORMAT_WITH_COLOR("%s:%d ", "3"), base_name, line);
        printf(FORMAT_WITH_COLOR("%s() ", "33"), func);
    } else {
        printf("%ld %s:%d %s() ", gettid_cached(), base_name, line, func);
    }
#endif

    fwrite_unlocked(start.value, start.len, 1, stdout);
    vprintf(fmt, values);

    if (UNLIKELY(type & STATUS_PERROR)) {
        char errbuf[64];
        char *errmsg =
            strerror_thunk_r(saved_errno, errbuf, sizeof(errbuf) - 1);

        printf(": %s (error number %d)", errmsg, saved_errno);
    }

    fwrite_unlocked(end.value, end.len, 1, stdout);

    funlockfile(stdout);

    errno = saved_errno;
}

#undef FORMAT_WITH_COLOR

#ifdef NDEBUG
#define IMPLEMENT_FUNCTION(fn_name_, type_)                                    \
    void lwan_status_##fn_name_(const char *fmt, ...)                          \
    {                                                                          \
        if (LIKELY(!quiet)) {                                                  \
            va_list values;                                                    \
            va_start(values, fmt);                                             \
            status_out(type_, fmt, values);                                    \
            va_end(values);                                                    \
        }                                                                      \
        if (UNLIKELY((type_)&STATUS_CRITICAL))                                 \
            exit(1);                                                           \
    }
#else
#define IMPLEMENT_FUNCTION(fn_name_, type_)                                    \
    void lwan_status_##fn_name_##_debug(const char *file, const int line,      \
                                        const char *func, const char *fmt,     \
                                        ...)                                   \
    {                                                                          \
        if (LIKELY(!quiet)) {                                                  \
            va_list values;                                                    \
            va_start(values, fmt);                                             \
            status_out(file, line, func, type_, fmt, values);                  \
            va_end(values);                                                    \
        }                                                                      \
        if (UNLIKELY((type_)&STATUS_CRITICAL))                                 \
            abort();                                                           \
    }

IMPLEMENT_FUNCTION(debug, STATUS_DEBUG)
#endif

IMPLEMENT_FUNCTION(info, STATUS_INFO)
IMPLEMENT_FUNCTION(warning, STATUS_WARNING)
IMPLEMENT_FUNCTION(error, STATUS_ERROR)
IMPLEMENT_FUNCTION(perror, STATUS_PERROR)

IMPLEMENT_FUNCTION(critical, STATUS_CRITICAL)
IMPLEMENT_FUNCTION(critical_perror, STATUS_CRITICAL | STATUS_PERROR)

#undef IMPLEMENT_FUNCTION
