// Invariants over servermsg.h - the message model both FrostServer and FrostMod speak.
//
// The two halves are built from the same header but run in different processes on different
// machines, so a disagreement about the feed format shows up as "the server says nothing",
// which looks exactly like a server with no announcements configured. The round trip is
// proved here instead.
//
// Pure logic, no Win32 and no game, so like fairsend_test.cpp it builds and runs anywhere.

#include "../src/servermsg.h"

#include <cmath>
#include <cstdio>
#include <string>

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

using namespace servermsg;

static Message Msg(uint32_t seq, const char* text, uint32_t rgb, Style st, float secs) {
    Message m;
    m.seq = seq; m.text = text; m.rgb = rgb; m.style = st; m.seconds = secs;
    return m;
}

int main() {
    {   // colours
        uint32_t v = 0;
        CHECK(ParseHexRgb("FF3B00", v) && v == 0xFF3B00u, "plain hex");
        CHECK(ParseHexRgb("#00ff80", v) && v == 0x00FF80u, "leading hash, lower case");
        CHECK(!ParseHexRgb("FF3B0", v), "five digits is not a colour");
        CHECK(!ParseHexRgb("GG3B00", v), "non-hex is not a colour");
        CHECK(!ParseHexRgb("", v), "empty is not a colour");
    }

    {   // styles round trip by name
        CHECK(StyleFromName("pulse")   == Style::Pulse,   "pulse");
        CHECK(StyleFromName("rainbow") == Style::Rainbow, "rainbow");
        CHECK(StyleFromName("fade")    == Style::Fade,    "fade");
        CHECK(StyleFromName("nonsense") == Style::None,   "an unknown style is plain, not an error");
        CHECK(std::string(StyleName(Style::Rainbow)) == "rainbow", "name back out");
    }

    {   // dwell is clamped, including the NaN a bad config can produce
        CHECK(ClampSeconds(8.0f) == 8.0f, "in range is untouched");
        CHECK(ClampSeconds(0.1f) == kMinSeconds, "too short is raised");
        CHECK(ClampSeconds(900.0f) == kMaxSeconds, "too long is cut");
        CHECK(ClampSeconds(std::nanf("")) == kDefaultSeconds, "NaN falls back to the default");
        CHECK(ClampSeconds(-3.0f) == kDefaultSeconds, "negative falls back to the default");
    }

    {   // icons
        auto spans = Tokenise(":flag: go");
        CHECK(spans.size() == 2, "icon then text, got %zu", spans.size());
        CHECK(spans[0].icon && spans[0].iconIndex == IconIndex("flag"), "the flag");
        CHECK(!spans[1].icon && spans[1].text == " go", "the rest, verbatim");

        spans = Tokenise("no icons here");
        CHECK(spans.size() == 1 && !spans[0].icon, "plain text is one span");

        spans = Tokenise("lap :clock: 2:30 left");
        CHECK(spans.size() == 3, "a time is not a token, got %zu", spans.size());
        CHECK(spans[1].icon, "the clock is");
        CHECK(spans[2].text == " 2:30 left", "and 2:30 survives");

        spans = Tokenise(":nosuchicon: hi");
        CHECK(spans.size() == 1 && !spans[0].icon, "an unknown name is left as typed");

        CHECK(IconIndex("trophy") >= 0, "trophy exists");
        CHECK(IconIndex("banana") == -1, "banana does not");

        size_t n = 0;
        const Icon* all = Icons(n);
        CHECK(n == 10, "ten icons, got %zu", n);
        for (size_t i = 0; i < n; ++i) {
            bool any = false;
            for (int r = 0; r < kIconSize; ++r) any = any || all[i].rows[r] != 0;
            CHECK(any, "icon '%s' is not blank", all[i].name);
        }
    }

    {   // cell counting drives the rainbow spread
        CHECK(CellCount(Tokenise("abc")) == 3, "three characters");
        CHECK(CellCount(Tokenise(":star:")) == 1, "an icon is one cell");
        CHECK(CellCount(Tokenise(":star:ab")) == 3, "icon plus two");
    }

    {   // the dwell ramp
        Message m = Msg(1, "hi", 0xFFFFFF, Style::None, 8.0f);
        m.shownAt = 100.0;
        CHECK(DwellAlpha(m, 100.0) == 0.0f, "nothing at the instant it arrives");
        CHECK(DwellAlpha(m, 100.0 + kFadeInSec) > 0.99f, "full once faded in");
        CHECK(DwellAlpha(m, 104.0) > 0.99f, "full in the middle");
        CHECK(DwellAlpha(m, 108.0) == 0.0f, "gone at the end of its dwell");
        CHECK(DwellAlpha(m, 200.0) == 0.0f, "and stays gone");
        const float late = DwellAlpha(m, 107.75);
        CHECK(late > 0.4f && late < 0.6f, "half way through the fade out, got %.3f", late);
    }

    {   // colour per style
        Message m = Msg(1, "hello", 0xFF3B00, Style::None, 8.0f);
        m.shownAt = 0.0;
        Rgba c = ColorAt(m, 0, 5, 1.0);
        CHECK(std::fabs(c.r - 1.0f) < 0.01f, "red channel is as authored, got %.3f", c.r);
        CHECK(std::fabs(c.g - 0x3B / 255.0f) < 0.01f, "green too");
        CHECK(std::fabs(c.b - 0.0f) < 0.01f, "and blue");

        m.style = Style::Rainbow;
        Rgba a = ColorAt(m, 0, 8, 1.0);
        Rgba b = ColorAt(m, 4, 8, 1.0);
        CHECK(std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) > 0.1f,
              "a rainbow differs along the line");

        m.style = Style::Pulse;
        Rgba p1 = ColorAt(m, 0, 5, 1.0);
        Rgba p2 = ColorAt(m, 0, 5, 1.55);
        CHECK(std::fabs(p1.r - p2.r) > 0.01f, "a pulse differs over time");
        CHECK(p1.r <= 1.0f && p2.r <= 1.0f, "and never exceeds the authored colour");

        m.style = Style::Fade;
        Rgba f1 = ColorAt(m, 0, 5, 1.0);
        Rgba f2 = ColorAt(m, 0, 5, 1.9);
        CHECK(std::fabs(f1.a - f2.a) > 0.01f, "a fade moves alpha, not colour");
        CHECK(std::fabs(f1.r - f2.r) < 0.001f, "its colour holds still");
    }

    {   // the feed round trips, text and all
        std::vector<Message> in = {
            Msg(7, ":flag: Welcome \"home\"", 0xFF3B00, Style::Pulse, 8.0f),
            Msg(8, "braces {here} and a \\ slash", 0x00FF80, Style::Rainbow, 12.0f),
        };
        const std::string json = BuildFeedJson(in, 8);

        std::vector<Message> out;
        uint32_t head = 0;
        CHECK(ParseFeedJson(json, out, head), "the feed parses");
        CHECK(head == 8, "head seq survived, got %u", head);
        CHECK(out.size() == 2, "both messages, got %zu", out.size());
        if (out.size() == 2) {
            CHECK(out[0].seq == 7 && out[0].rgb == 0xFF3B00u, "first message's fields");
            CHECK(out[0].style == Style::Pulse, "its style");
            CHECK(out[0].text == ":flag: Welcome \"home\"", "its text, quotes intact: %s",
                  out[0].text.c_str());
            CHECK(out[1].text == "braces {here} and a \\ slash",
                  "a brace in the text did not split the object: %s", out[1].text.c_str());
            CHECK(std::fabs(out[1].seconds - 12.0f) < 0.01f, "its dwell");
        }
    }

    {   // a feed we cannot read yields nothing rather than nonsense
        std::vector<Message> out;
        uint32_t head = 0;
        CHECK(!ParseFeedJson("not json at all", out, head), "garbage is rejected");
        CHECK(out.empty(), "and leaves no messages");
        CHECK(ParseFeedJson("{\"seq\":3,\"messages\":[]}", out, head), "an empty feed is valid");
        CHECK(out.empty() && head == 3, "with just a head seq");
        // A message with no text is not a message.
        CHECK(ParseFeedJson("{\"seq\":4,\"messages\":[{\"seq\":4}]}", out, head), "parses");
        CHECK(out.empty(), "but the textless entry is dropped");
    }

    {   // defaults survive a server that sends less than we expect
        std::vector<Message> out;
        uint32_t head = 0;
        ParseFeedJson("{\"seq\":2,\"messages\":[{\"seq\":2,\"text\":\"bare\"}]}", out, head);
        CHECK(out.size() == 1, "the bare message came through");
        if (out.size() == 1) {
            CHECK(out[0].style == Style::None, "with a plain style");
            CHECK(out[0].rgb == 0xFFFFFFu, "and the default colour");
            CHECK(std::fabs(out[0].seconds - kDefaultSeconds) < 0.01f, "and the default dwell");
        }
    }

    {   // the queue
        Queue q;
        CHECK(q.Empty() && q.LastSeq() == 0, "starts empty");

        q.Ingest({ Msg(1, "a", 0xFFFFFF, Style::None, 8.0f),
                   Msg(2, "b", 0xFFFFFF, Style::None, 8.0f) }, 10.0);
        CHECK(q.Live().size() == 2 && q.LastSeq() == 2, "two in");

        // The same poll arriving twice must not double the screen.
        q.Ingest({ Msg(1, "a", 0xFFFFFF, Style::None, 8.0f),
                   Msg(2, "b", 0xFFFFFF, Style::None, 8.0f) }, 11.0);
        CHECK(q.Live().size() == 2, "a repeated poll adds nothing, got %zu", q.Live().size());

        q.Ingest({ Msg(3, "c", 0xFFFFFF, Style::None, 2.0f) }, 11.0);
        CHECK(q.Live().size() == 3 && q.LastSeq() == 3, "a newer one is taken");

        q.Expire(14.0);   // seq 3 was 2s long from t=11
        CHECK(q.Live().size() == 2, "the short one expired, got %zu", q.Live().size());
        q.Expire(30.0);
        CHECK(q.Empty(), "and eventually all of them");

        // A burst longer than the screen keeps the newest.
        Queue b;
        std::vector<Message> burst;
        for (uint32_t i = 1; i <= kMaxLive + 3; ++i)
            burst.push_back(Msg(i, std::to_string(i).c_str(), 0xFFFFFF, Style::None, 20.0f));
        b.Ingest(burst, 0.0);
        CHECK(b.Live().size() == kMaxLive, "capped at %zu, got %zu", kMaxLive, b.Live().size());
        CHECK(b.Live().back().seq == kMaxLive + 3, "and it is the newest that survived");
        CHECK(b.Live().front().seq == 4, "the oldest went, got %u", b.Live().front().seq);

        b.Reset();
        CHECK(b.Empty() && b.LastSeq() == 0, "a reset forgets the old server's sequence");
    }

    {   // long text is cut rather than run off the screen
        const std::string huge(kMaxText + 50, 'x');
        CHECK(Truncate(huge).size() == kMaxText, "truncated to %zu", kMaxText);
        std::vector<Message> out;
        uint32_t head = 0;
        ParseFeedJson("{\"seq\":1,\"messages\":[{\"seq\":1,\"text\":\"" + huge + "\"}]}", out, head);
        CHECK(out.size() == 1 && out[0].text.size() == kMaxText, "and on the way in too");
    }

    if (g_failures == 0) std::printf("servermsg_test: all checks passed\n");
    else                 std::printf("servermsg_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
