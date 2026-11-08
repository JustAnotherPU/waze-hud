// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Host test for send_gate.h: one picture per interval (0: all), and a send
 * on a change, on being shown and every keep_ms otherwise. */
#include <cstdio>

#include "../src/send_gate.h"

using namespace woc;

static int failures;
static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

int main()
{
    PictureGate p;
    ok("picture: first frame", p.due(1000, 66));
    p.made(1000, 66);
    ok("picture: held inside the interval", !p.due(1030, 66));
    ok("picture: due after the interval", p.due(1066, 66));
    ok("picture: interval 0 takes every frame", p.due(1001, 0));
    ok("picture: clock going back is due", p.due(500, 66));
    int made = 0;
    PictureGate q;
    for (int t = 0; t < 1000; t += 45)   /* ~22 fps in */
        if (q.due(t, 66)) { q.made(t, 66); made++; }
    ok("picture: 22 fps in, 15 a second out", made >= 14 && made <= 16);
    int hid = 0;
    PictureGate h;
    for (int t = 0; t < 10000; t += 66)   /* 15 fps in, 5 wanted */
        if (h.due(t, 200)) { h.made(t, 200); hid++; }
    ok("picture: 15 fps in, 5 a second out", hid >= 49 && hid <= 51);

    SendGate s(500);
    ok("send: first picture", s.send(0, false, false));
    ok("send: unchanged held", !s.send(100, false, false));
    ok("send: changed sent", s.send(200, true, false));
    ok("send: just shown sent", s.send(250, false, true));
    ok("send: still shown, unchanged held", !s.send(300, false, true));
    ok("send: keep_ms refresh", s.send(750, false, true));
    ok("send: held counted", s.held() == 2);
    SendGate z(0);
    ok("send: a layer without a frame is fed at once", s.send(800, false, true, true));
    ok("send: keep_ms 0 sends all", z.send(0, false, false) && z.send(1, false, false));
    return failures ? 1 : 0;
}
