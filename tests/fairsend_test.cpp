// Invariants over the promotion rule in fairsend.h.
//
// This runs on the real candidate array inside a live dedicated server, reordering records the
// game is about to read. Getting it wrong does not crash anything, which is the problem: it
// would quietly send the wrong riders and the symptom would be indistinguishable from the bug
// it is meant to fix. So the ordering is proved here rather than eyeballed on a server.
//
// Pure logic, no Win32 and no game, so like offsets_test.cpp it builds and runs anywhere.

#include "../src/fairsend.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

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

using namespace fairsend;

// A filled record: `id` rides in the index field so a test can name it after a reorder, and
// `stale` is how many ticks behind it is (the record itself stores the negation).
static Record Rec(int32_t id, int stale) {
    Record r{};
    r.index     = id;
    r.slot      = 0x1000 + (uint64_t)id;   // any non-null pointer means "filled"
    r.tickDelta = -stale;
    r.sortKey   = (float)id;
    return r;
}

// The ids left in the array, in order, as a string like "3,1,2".
static void IdsOf(const Record* r, int n, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        char one[16];
        std::snprintf(one, sizeof(one), i ? ",%d" : "%d", r[i].index);
        std::strncat(out, one, cap - std::strlen(out) - 1);
    }
}

static void CheckOrder(const char* what, Record* recs, int n, int promoteAt,
                       int wantMoved, const char* wantIds) {
    const int moved = Promote(recs, n, promoteAt);
    char got[128];
    IdsOf(recs, n, got, sizeof(got));
    CHECK(moved == wantMoved, "%s: promoted %d, want %d", what, moved, wantMoved);
    CHECK(std::strcmp(got, wantIds) == 0, "%s: order %s, want %s", what, got, wantIds);
}

int main() {
    // --- the layout the game dictates -------------------------------------------------
    CHECK(sizeof(Record) == 0x20, "record must be 32 bytes");
    CHECK(offsetof(Record, slot)      == 0x08, "slot pointer at +0x08");
    CHECK(offsetof(Record, tickDelta) == 0x10, "tick delta at +0x10");
    CHECK(offsetof(Record, sortKey)   == 0x14, "sort key at +0x14");
    CHECK(kPromoteAtTicks < kCliffTicks, "we must act before the client gives up");
    CHECK(kPromoteAtTicks == 9 && kCliffTicks == 10, "300ms at 30ms a tick, promote one under");

    // --- staleness, and its sign ------------------------------------------------------
    // The record stores rider_tick - current_tick, so silence reads negative.
    CHECK(StalenessTicks(Rec(0, 0))  == 0,  "a rider sent this tick is not stale");
    CHECK(StalenessTicks(Rec(0, 7))  == 7,  "seven ticks behind reads seven");
    CHECK(StalenessTicks(Rec(0, 10)) == 10, "at the cliff reads ten");
    {   // A positive delta would be a tick in the future. Clamp rather than trust it.
        Record r = Rec(0, 0); r.tickDelta = 5;
        CHECK(StalenessTicks(r) == 0, "a future tick clamps to zero, not a negative staleness");
    }
    {   // Do not let an absurd value wrap when negated.
        Record r = Rec(0, 0); r.tickDelta = INT32_MIN;
        CHECK(StalenessTicks(r) > 0, "INT32_MIN must not negate into a negative");
    }

    // --- counting the filled prefix ---------------------------------------------------
    {
        Record a[5] = { Rec(1,0), Rec(2,0), Rec(3,0), {}, {} };
        CHECK(CountFilled(a, 5) == 3, "counts up to the first null slot");
        CHECK(CountFilled(a, 2) == 2, "respects the cap it is given");
        CHECK(CountFilled(nullptr, 5) == 0, "null array counts zero");
        CHECK(CountFilled(a, 0) == 0, "zero cap counts zero");
        Record empty[2] = { {}, {} };
        CHECK(CountFilled(empty, 2) == 0, "an untouched array counts zero");
    }
    {
        // The case that matters. The builder drops a throttled rider by memmoving the rest
        // down one and decrementing its own count, which leaves the last record duplicated in
        // the tail with a live-looking pointer. Counting on null alone would walk into it.
        Record a[5] = { Rec(1,0), Rec(2,0), Rec(3,0), {}, {} };
        a[3] = a[2];                      // exactly what the memmove leaves behind
        CHECK(CountFilled(a, 5) == 3, "a duplicated tail record ends the prefix");

        // Two drops leave two copies, and the prefix still ends where the live riders do.
        Record c[6] = { Rec(1,0), Rec(2,0), Rec(3,0), {}, {}, {} };
        c[3] = c[2];
        c[4] = c[2];
        CHECK(CountFilled(c, 6) == 3, "two duplicated tail records still end the prefix");

        // A duplicate of the FIRST record, not the last, must end it just the same.
        Record d[4] = { Rec(1,0), Rec(2,0), {}, {} };
        d[2] = d[0];
        CHECK(CountFilled(d, 4) == 2, "any repeat ends the prefix, not just the last one");

        // A full array of genuinely distinct riders must not be cut short by this check.
        Record full[kMaxRecords];
        for (size_t i = 0; i < kMaxRecords; ++i) full[i] = Rec((int32_t)i, 0);
        CHECK(CountFilled(full, (int)kMaxRecords) == (int)kMaxRecords,
              "50 distinct riders all count");
    }

    // --- the threshold ----------------------------------------------------------------
    CHECK(!NeedsPromotion(Rec(0, 8), kPromoteAtTicks), "eight ticks can still wait");
    CHECK(NeedsPromotion(Rec(0, 9), kPromoteAtTicks),  "nine ticks must go now");
    CHECK(NeedsPromotion(Rec(0, 40), kPromoteAtTicks), "long past the cliff still goes");

    // --- promotion: the case this exists for ------------------------------------------
    {   // Riders 1 and 2 are near and fresh. Rider 3 is far and about to vanish.
        Record a[3] = { Rec(1,0), Rec(2,1), Rec(3,9) };
        CheckOrder("the starving rider goes first", a, 3, kPromoteAtTicks, 1, "3,1,2");
    }
    {   // Two starving riders keep their distance order relative to each other.
        Record a[5] = { Rec(1,0), Rec(2,9), Rec(3,0), Rec(4,12), Rec(5,0) };
        CheckOrder("stable within both groups", a, 5, kPromoteAtTicks, 2, "2,4,1,3,5");
    }
    {   // Nobody is starving, so the array must be left exactly as it was.
        Record a[4] = { Rec(1,0), Rec(2,3), Rec(3,8), Rec(4,1) };
        CheckOrder("no starving riders is a no-op", a, 4, kPromoteAtTicks, 0, "1,2,3,4");
    }
    {   // Everybody is starving. There is no one to promote them ahead of, so leave it be.
        Record a[3] = { Rec(1,9), Rec(2,11), Rec(3,20) };
        CheckOrder("all starving is a no-op", a, 3, kPromoteAtTicks, 0, "1,2,3");
    }
    {   // The starving rider is already at the front.
        Record a[3] = { Rec(1,9), Rec(2,0), Rec(3,0) };
        CheckOrder("already in front stays in front", a, 3, kPromoteAtTicks, 1, "1,2,3");
    }
    {   // The whole tail is starving and must arrive in its original order.
        Record a[5] = { Rec(1,0), Rec(2,0), Rec(3,9), Rec(4,9), Rec(5,9) };
        CheckOrder("a starving tail keeps its order", a, 5, kPromoteAtTicks, 3, "3,4,5,1,2");
    }

    // --- promotion refuses the degenerate ---------------------------------------------
    {
        Record a[1] = { Rec(1, 40) };
        CHECK(Promote(a, 1, kPromoteAtTicks) == 0, "one record cannot be reordered");
        CHECK(Promote(nullptr, 8, kPromoteAtTicks) == 0, "null array is refused");
        CHECK(Promote(a, 0, kPromoteAtTicks) == 0, "empty is refused");
        CHECK(Promote(a, -3, kPromoteAtTicks) == 0, "a negative count is refused");
    }
    {   // A count past the array cap is clamped rather than walked off the end.
        Record a[kMaxRecords];
        for (size_t i = 0; i < kMaxRecords; ++i) a[i] = Rec((int32_t)i, i == kMaxRecords - 1 ? 9 : 0);
        const int moved = Promote(a, (int)kMaxRecords + 999, kPromoteAtTicks);
        CHECK(moved == 1, "a count past the cap is clamped, not trusted");
        CHECK(a[0].index == (int32_t)kMaxRecords - 1, "the last rider was promoted to the front");
    }

    // --- nothing is lost or duplicated ------------------------------------------------
    {   // Every id that went in must come out exactly once, whatever the order.
        Record a[8];
        for (int i = 0; i < 8; ++i) a[i] = Rec(i, (i % 3 == 0) ? 11 : 2);
        Promote(a, 8, kPromoteAtTicks);
        int seen[8] = {0};
        for (int i = 0; i < 8; ++i) {
            CHECK(a[i].index >= 0 && a[i].index < 8, "an id survived the move intact");
            if (a[i].index >= 0 && a[i].index < 8) seen[a[i].index]++;
        }
        for (int i = 0; i < 8; ++i) CHECK(seen[i] == 1, "id %d appears exactly once", i);
    }
    {   // A moved record carries the game's own fields with it, untouched.
        Record a[2] = { Rec(1, 0), Rec(2, 9) };
        a[1].tail = 0xDEADBEEFCAFEF00Dull;
        a[1].pad04 = 0x12345678u;
        Promote(a, 2, kPromoteAtTicks);
        CHECK(a[0].index == 2, "the starving rider moved");
        CHECK(a[0].tail == 0xDEADBEEFCAFEF00Dull, "its tail came with it");
        CHECK(a[0].pad04 == 0x12345678u, "and so did the field we do not read");
    }

    if (g_failures == 0) std::printf("fairsend_test: all checks passed\n");
    else                 std::printf("fairsend_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
