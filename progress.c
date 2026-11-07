#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Progress {
    int tty;
    uint64_t start, last;
    int last_pct;
    int width;
};

char *human(int64_t n)
{
    char b[64];
    double f = (double)n;
    if (f >= 1073741824.0) snprintf(b, sizeof b, "%.2fG", f / 1073741824.0);
    else if (f >= 1048576.0) snprintf(b, sizeof b, "%.2fM", f / 1048576.0);
    else if (f >= 1024.0) snprintf(b, sizeof b, "%.2fK", f / 1024.0);
    else snprintf(b, sizeof b, "%lldB", (long long)n);
    return str_dup(b);
}

Progress *progress_new(void)
{
    Progress *p = (Progress *)xmalloc(sizeof *p);
    if (!p) return NULL;
    p->tty = stdout_is_console();
    p->start = p->last = tick_ms();
    p->last_pct = 0;
    p->width = 28;
    return p;
}

void progress_free(Progress *p) { free(p); }

void progress_update(Progress *p, int64_t done, int64_t total)
{
    int pct = 0, filled;
    uint64_t now;
    double elapsed, rate;
    char *sdone, *stotal, *sspeed;
    if (!p) return;
    if (total > 0) pct = (int)(done * 100 / total);
    if (pct > 100) pct = 100;
    now = tick_ms();
    if (p->tty) {
        if (now - p->last < 80 && pct < 100) return;
    } else if (pct < 100 && pct - p->last_pct < 5) {
        return;
    }
    p->last = now;
    p->last_pct = pct;
    elapsed = (double)(now - p->start) / 1000.0;
    rate = elapsed > 0 ? (double)done / elapsed : 0;
    sdone = human(done);
    stotal = human(total);
    sspeed = elapsed > 0 ? human((int64_t)rate) : str_dup("-");
    if (!sdone || !stotal || !sspeed) goto done;
    if (p->tty) {
        Buf bar;
        if (buf_init(&bar, 128)) goto done;
        filled = total > 0 ? (int)(done * (int64_t)p->width / total) : 0;
        if (filled > p->width) filled = p->width;
        for (int i = 0; i < filled; i++) buf_append_cstr(&bar, "█");
        for (int i = filled; i < p->width; i++) buf_append_cstr(&bar, "░");
        out("\r[%s] %3d%%  %s%s  %s/%s", bar.data, pct, sspeed,
            elapsed > 0 ? "/s" : "", sdone, stotal);
        buf_free(&bar);
    } else {
        out("%3d%%  %s/%s  %s%s\n", pct, sdone, stotal, sspeed,
            elapsed > 0 ? "/s" : "");
    }
done:
    free(sdone); free(stotal); free(sspeed);
}

void progress_clear(Progress *p)
{
    if (p && p->tty) out("\r%s\r",
        "                                                                                                    ");
}
