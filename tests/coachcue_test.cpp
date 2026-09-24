// MXB Coach's live cues (src/coachcue.h).
//
// The .cue file is a wire contract with MXB Coach's Rust writer, compiled separately, so its
// shape is pinned here, with the rules for when a cue shows: before its spot at the bike's
// speed, a few a lap at most, and only in practice. Pure C++, runs anywhere.

#include "../src/coachcue.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

struct In {
    float       at;
    uint8_t     kind;
    uint8_t     priority;
    std::string text;
};

// Writes a sheet the way the app does.
static std::vector<uint8_t> Sheet(float len, const std::vector<In>& cues, float lead = 1.2f, float show = 1.5f,
                                  float gap = 3.0f, uint32_t max = 4) {
    std::vector<uint8_t> b(coachcue::kMagic, coachcue::kMagic + 4);
    auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    uint32_t v = coachcue::kVersion, count = uint32_t(cues.size());
    put(&v, 4);
    put(&len, 4);
    put(&lead, 4);
    put(&show, 4);
    put(&gap, 4);
    put(&max, 4);
    put(&count, 4);
    for (const In& c : cues) {
        uint16_t tl = uint16_t(c.text.size());
        put(&c.at, 4);
        b.push_back(c.kind);
        b.push_back(c.priority);
        put(&tl, 2);
        put(c.text.data(), c.text.size());
    }
    return b;
}

static bool Load(const std::vector<uint8_t>& b, coachcue::Sheet& s) { return coachcue::Parse(b.data(), b.size(), s); }

static void ReadsWhatTheAppWrites() {
    coachcue::Sheet s;
    CHECK(Load(Sheet(1650, {{900, coachcue::THROTTLE, 2, "Hold the gas"}, {180, coachcue::BRAKE, 5, "Brake"}}), s), "parse");
    CHECK(s.track_len == 1650, "length %f", s.track_len);
    CHECK(s.cues.size() == 2, "count %zu", s.cues.size());
    CHECK(s.cues[0].at_m == 180 && s.cues[0].kind == coachcue::BRAKE && s.cues[0].text == "Brake", "sorted by position");
    CHECK(s.cues[1].priority == 2, "priority kept");
    CHECK(s.max_per_lap == 4 && s.lead_s > 1.1f && s.lead_s < 1.3f, "settings kept");
}

static void RefusesWhatTheAppDoesNotWrite() {
    coachcue::Sheet s;
    std::vector<uint8_t> ok = Sheet(1000, {{100, coachcue::BRAKE, 1, "Brake"}});
    std::vector<uint8_t> bad = ok;
    bad[0] = 'X';
    CHECK(!Load(bad, s), "wrong magic");
    bad = ok;
    bad[4] = 2;
    CHECK(!Load(bad, s), "wrong version");
    CHECK(!Load(std::vector<uint8_t>(ok.begin(), ok.end() - 2), s), "cut short");
    CHECK(!Load(Sheet(1000, {{1200, coachcue::BRAKE, 1, "Brake"}}), s), "cue past the end of the track");
    CHECK(!Load(Sheet(1000, {{100, coachcue::BRAKE, 1, std::string(120, 'x')}}), s), "text too long to draw");
    std::vector<In> many(65, In{10, coachcue::BRAKE, 1, "Brake"});
    CHECK(!Load(Sheet(1000, many), s), "too many cues");
    CHECK(Load(Sheet(1000, {{100, coachcue::BRAKE, 1, "Brake"}}, 1.2f, 1.5f, 3.0f, 999), s) && s.max_per_lap == 16,
          "a silly cap is clamped");
}

static void OnlyInPractice() {
    CHECK(coachcue::Practice(1, 1), "testing");
    CHECK(coachcue::Practice(2, 1), "race event practice");
    CHECK(!coachcue::Practice(2, 4), "qualify");
    CHECK(!coachcue::Practice(2, 6), "race 1");
    CHECK(!coachcue::Practice(2, 7), "race 2");
    CHECK(!coachcue::Practice(4, 1), "straight rhythm");
}

static void ReadsTheEventAndNamesTheFiles() {
    std::vector<uint8_t> ev(820, 0);
    std::memcpy(ev.data() + coachcue::kEventBikeId, "MX2OEM_2023_KTM_250_SX-F", 24);
    std::memcpy(ev.data() + coachcue::kEventTrackId, "indiana nationals", 17);
    float len = 1650;
    int type = 1;
    std::memcpy(ev.data() + coachcue::kEventTrackLength, &len, 4);
    std::memcpy(ev.data() + coachcue::kEventType, &type, 4);
    coachcue::Event e = coachcue::ReadEvent(ev.data(), int(ev.size()));
    CHECK(e.track == "indiana nationals" && e.bike == "MX2OEM_2023_KTM_250_SX-F", "ids");
    CHECK(e.track_len == 1650 && e.type == 1 && e.server.empty(), "length, type, offline");
    std::vector<std::string> names = coachcue::SheetNames(e);
    CHECK(names.size() == 2, "two names");
    CHECK(names[0] == "indiana_nationals.MX2OEM_2023_KTM_250_SX-F.cue", "bike first: %s", names[0].c_str());
    CHECK(names[1] == "indiana_nationals.cue", "then the track: %s", names[1].c_str());
    coachcue::Sheet s;
    Load(Sheet(1650.4f, {{100, coachcue::BRAKE, 1, "Brake"}}), s);
    CHECK(coachcue::Fits(s, e), "same track length");
    Load(Sheet(1400, {{100, coachcue::BRAKE, 1, "Brake"}}), s);
    CHECK(!coachcue::Fits(s, e), "another track's sheet");
}

// Rides a lap at a steady speed from `from` to `to` metres, 50 Hz, calling back each sample.
template <typename F>
static void Ride(coachcue::Player& p, float len, float from, float to, float speed, float& t, F each) {
    for (float m = from; m < to; m += speed * 0.02f, t += 0.02f) {
        p.on_sample(m / len, speed, t, false);
        each(m);
    }
}

static void ShowsBeforeTheSpotAtSpeed() {
    coachcue::Sheet s;
    Load(Sheet(1000, {{500, coachcue::BRAKE, 1, "Brake"}}), s);
    coachcue::Player p;
    p.load(s);
    p.set_practice(true);
    float t = 0, shown_at = -1;
    // 20 m/s with a 1.2 s lead: up about 24 m before the spot.
    Ride(p, 1000, 0, 600, 20, t, [&](float m) {
        if (shown_at < 0 && p.showing()) shown_at = m;
    });
    CHECK(shown_at > 474 && shown_at < 478, "shown at %f m", shown_at);
    CHECK(!p.showing(), "gone after its time");
}

static void NothingOutsidePractice() {
    coachcue::Sheet s;
    Load(Sheet(1000, {{500, coachcue::BRAKE, 1, "Brake"}}), s);
    coachcue::Player p;
    p.load(s);
    float t = 0;
    bool seen = false;
    Ride(p, 1000, 0, 600, 20, t, [&](float) { seen |= p.showing() != nullptr; });
    CHECK(!seen, "cues in a race");
}

static void AFewALapAndSpacedOut() {
    coachcue::Sheet s;
    // Five cues, 20 m apart, cap 2 a lap, 3 s apart. At 20 m/s "a" shows 20 m early, at 180 m;
    // the gap holds everything until 240 m, where "b" is already passed and "c" is at its spot.
    // Two shown, the cap.
    Load(Sheet(1000, {{200, 3, 1, "a"}, {220, 3, 1, "b"}, {240, 3, 1, "c"}, {260, 3, 1, "d"}, {280, 3, 1, "e"}},
               1.0f, 0.5f, 3.0f, 2),
         s);
    coachcue::Player p;
    p.load(s);
    p.set_practice(true);
    float t = 0;
    std::string order;
    const coachcue::Cue* last = nullptr;
    Ride(p, 1000, 0, 900, 20, t, [&](float) {
        const coachcue::Cue* c = p.showing();
        if (c && c != last) order += c->text;
        last = c;
    });
    CHECK(order == "ac", "shown %s", order.c_str());
}

static void TheMoreImportantOneWins() {
    coachcue::Sheet s;
    Load(Sheet(1000, {{300, coachcue::STAND, 1, "Stand up"}, {302, coachcue::BRAKE, 7, "Brake"}}), s);
    coachcue::Player p;
    p.load(s);
    p.set_practice(true);
    float t = 0;
    std::string first;
    Ride(p, 1000, 0, 400, 20, t, [&](float) {
        if (first.empty() && p.showing()) first = p.showing()->text;
    });
    CHECK(first == "Brake", "first shown %s", first.c_str());
}

static void EveryLapAgain() {
    coachcue::Sheet s;
    Load(Sheet(1000, {{500, coachcue::BRAKE, 1, "Brake"}}), s);
    coachcue::Player p;
    p.load(s);
    p.set_practice(true);
    float t = 0;
    int shows = 0;
    const coachcue::Cue* last = nullptr;
    for (int lap = 0; lap < 3; ++lap) {
        Ride(p, 1000, 0, 1000, 25, t, [&](float) {
            const coachcue::Cue* c = p.showing();
            if (c && c != last) ++shows;
            last = c;
        });
    }
    CHECK(shows == 3, "shown %d times over 3 laps", shows);
}

static void ACrashClearsIt() {
    coachcue::Sheet s;
    Load(Sheet(1000, {{500, coachcue::BRAKE, 1, "Brake"}}), s);
    coachcue::Player p;
    p.load(s);
    p.set_practice(true);
    float t = 0;
    Ride(p, 1000, 0, 480, 20, t, [](float) {});
    CHECK(p.showing() != nullptr, "showing");
    p.on_sample(0.48f, 0, t + 0.02f, true);
    CHECK(!p.showing(), "cleared by the crash");
}

// A sheet that wasn't there when the track loaded is looked for while riding, not only at the
// next event. That was the bug: set a lap, quit, reload the track, and only then be coached.
static void AMissingSheetIsLookedForWhileRiding() {
    using coachcue::ShouldLook;
    CHECK(ShouldLook(false, false, 5000, 3000), "no sheet: look during the lap");
    CHECK(!ShouldLook(false, false, 3500, 3000), "but no more than once a second");
    CHECK(ShouldLook(false, false, 4000, 3000), "a second on, look again");
    CHECK(!ShouldLook(true, false, 99000, 3000), "a sheet in use is not swapped mid-lap");
    CHECK(ShouldLook(true, true, 3001, 3000), "it is swapped at the line");
    CHECK(ShouldLook(false, true, 3001, 3000), "and a missing one is looked for there too");
    CHECK(ShouldLook(false, false, 100, 3000), "a clock that went backwards doesn't stall the look");
}

// Taken mid-lap, a sheet's cues still ahead fire and the ones already passed wait for the next
// lap, rather than all going off at once.
static void ASheetTakenMidLapStartsFromHere() {
    coachcue::Sheet s;
    Load(Sheet(1000, {{200, coachcue::BRAKE, 1, "Brake"}, {700, coachcue::THROTTLE, 1, "Gas"}}), s);
    coachcue::Player p;
    p.set_practice(true);
    float t = 0;
    Ride(p, 1000, 0, 400, 20, t, [](float) {});
    p.load(s);
    std::string order;
    const coachcue::Cue* last = nullptr;
    auto note = [&](float) {
        const coachcue::Cue* c = p.showing();
        if (c && c != last) order += c->text + ";";
        last = c;
    };
    Ride(p, 1000, 400, 1000, 20, t, note);
    CHECK(order == "Gas;", "the rest of this lap: %s", order.c_str());
    order.clear();
    Ride(p, 1000, 0, 1000, 20, t, note);
    CHECK(order == "Brake;Gas;", "the whole of the next: %s", order.c_str());
}

int main() {
    AMissingSheetIsLookedForWhileRiding();
    ASheetTakenMidLapStartsFromHere();
    ReadsWhatTheAppWrites();
    RefusesWhatTheAppDoesNotWrite();
    OnlyInPractice();
    ReadsTheEventAndNamesTheFiles();
    ShowsBeforeTheSpotAtSpeed();
    NothingOutsidePractice();
    AFewALapAndSpacedOut();
    TheMoreImportantOneWins();
    EveryLapAgain();
    ACrashClearsIt();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachcue: all checks passed\n");
    return 0;
}
