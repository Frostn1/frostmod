// When FrostMod clears a wedged server browser by itself, and when it leaves it alone.
//
// The wedge — a half-open master login that makes the game's own Browse time out forever —
// and a master server that is genuinely down produce the same event inside the game: a login
// state going 2 -> 1 with reason 0. So the whole judgement is in the counters around it, and
// getting it wrong is expensive in both directions. Too eager and we rebind a socket every
// five seconds at a server that isn't answering anybody. Too cautious and the rider is told
// to press a button in another application to fix a bug we can see happening.
//
// None of that is reproducible on demand: it needs a licensed copy, a populated server and a
// browser that has already stuck. src/worldwatch.h keeps the deciding apart from the process
// so it can be proved here instead. Pure arithmetic, so it runs on any host.

#include "../src/worldwatch.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using namespace worldwatch;

/// A tick far enough past the last reset that the gap is never what a case is testing.
static uint64_t past(const State& s) { return s.nextResetOkMs + 1; }

int main() {
    // --- the ordinary wedge ------------------------------------------------------------
    // Logged in, rode, left the server, opened Browse: the first timeout is acted on at once,
    // and with the cheap reset, because the session is the likelier half and the login that
    // follows is what tells us which half it was.
    {
        State s;
        OnLogin(s);
        const Decision d = OnTimeout(s, 1000);
        CHECK(d.action == Action::Reset, "a timeout after a good login is the wedge");
        CHECK(!d.alsoPort, "the first reset of an episode keeps the source port");
        CHECK(s.resets == 1 && s.totalResets == 1, "resets %d total %d", s.resets, s.totalResets);
        CHECK(!s.everLogged, "a reset disarms until a fresh login");
    }

    // --- escalation inside one episode --------------------------------------------------
    // The session close didn't take. The next reset takes the port as well — but it costs two
    // timeouts to get there, because a reset disarms `everLogged` and the outage hold applies
    // again. That is deliberate: once we have reset it, we are back to not knowing whether
    // this machine is wedged or the master went out while we were working on it.
    {
        State s;
        OnLogin(s);
        OnTimeout(s, 1000);
        const uint64_t t = past(s);
        CHECK(OnTimeout(s, t).action == Action::HoldForOutage,
              "the first timeout after a reset is not evidence on its own");
        const Decision d = OnTimeout(s, t);
        CHECK(d.action == Action::Reset, "still trying inside the budget");
        CHECK(d.alsoPort, "the second reset of an episode takes the port too");
    }

    // --- the gap ------------------------------------------------------------------------
    // The game retries its login every five seconds; a reset needs longer than that to show
    // whether it worked. A timeout inside the gap costs nothing but a log line.
    {
        State s;
        OnLogin(s);
        OnTimeout(s, 1000);
        const uint64_t inside = 1000 + kResetGapMs - 5000;
        OnTimeout(s, inside);   // the outage hold, which comes first
        const Decision d = OnTimeout(s, inside);
        CHECK(d.action == Action::TooSoon, "inside the gap");
        CHECK(d.waitMs == 5000, "waitMs %llu", (unsigned long long)d.waitMs);
        CHECK(s.resets == 1, "a waited-out timeout spends no budget");
    }

    // --- a master that is actually down --------------------------------------------------
    // Nothing has logged in this run, so the first timeout is not evidence of anything. The
    // second one gets the benefit of the doubt, and then the budget runs out and we go quiet
    // rather than rebinding sockets at a server that isn't there.
    {
        State s;
        uint64_t t = 1000;
        CHECK(OnTimeout(s, t).action == Action::HoldForOutage, "first timeout of a cold run holds");
        CHECK(OnTimeout(s, t).action == Action::Reset, "the second one is tried");
        for (int i = 2; i <= kMaxAutoResets; ++i) {
            t = past(s);
            OnTimeout(s, t);    // the hold again, every time
            CHECK(OnTimeout(s, t).action == Action::Reset, "reset %d is inside the budget", i);
        }
        CHECK(s.resets == kMaxAutoResets, "budget spent: %d", s.resets);
        // The hold is tested before the budget, so the timeout straight after a reset still
        // reads as "not evidence yet" — then it is over budget from there on, however long
        // the outage runs.
        OnTimeout(s, past(s));
        CHECK(OnTimeout(s, past(s)).action == Action::OverBudget,
              "the run of timeouts stops at the limit");
        CHECK(OnTimeout(s, past(s) + 10 * kResetGapMs).action == Action::OverBudget,
              "and stays stopped however long it goes on");
    }

    // --- the refund, which is the point of the episode budget -----------------------------
    // An evening of joining and leaving servers wedges once per session. Every one of those is
    // its own episode: the rider who has already been fixed three times is the rider we have
    // the most evidence about, not the one to stop helping.
    {
        State s;
        for (int session = 1; session <= 6; ++session) {
            OnLogin(s);
            const Decision d = OnTimeout(s, (uint64_t)session * 10 * kResetGapMs);
            CHECK(d.action == Action::Reset, "session %d is fixed", session);
            CHECK(!d.alsoPort, "session %d starts from the cheap reset again", session);
            CHECK(s.resets == 1, "session %d spends one of its own budget", session);
        }
        CHECK(s.totalResets == 6, "the run's total keeps counting: %d", s.totalResets);
    }

    // A login part-way through an episode refunds it mid-flight, escalation included.
    {
        State s;
        OnLogin(s);
        OnTimeout(s, 1000);                       // cheap reset
        const uint64_t t = past(s);
        OnTimeout(s, t);                          // hold
        CHECK(OnTimeout(s, t).alsoPort, "escalated");
        OnLogin(s);                               // ...and it came back
        const Decision d = OnTimeout(s, 10 * kResetGapMs);
        CHECK(d.action == Action::Reset && !d.alsoPort, "the next wedge is a fresh episode");
        CHECK(s.totalResets == 3, "total %d", s.totalResets);
    }

    // --- the gap survives a refund --------------------------------------------------------
    // The budget is about belief; the gap is about physics. A login arriving does not make it
    // useful to reset twice inside five seconds, so the refund must not hand back the gap.
    {
        State s;
        OnLogin(s);
        OnTimeout(s, 1000);
        OnLogin(s);
        const Decision d = OnTimeout(s, 1000 + kResetGapMs - 1);
        CHECK(d.action == Action::TooSoon, "a refund does not shorten the gap");
        CHECK(d.waitMs == 1, "waitMs %llu", (unsigned long long)d.waitMs);
    }

    if (g_failures == 0) std::printf("worldwatch: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
