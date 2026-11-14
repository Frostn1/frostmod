#include "looksync.h"

#include <cassert>
#include <iostream>

static std::string Hash(const std::string& text) {
    looksync::Sha256 hash;
    unsigned char digest[32];
    hash.Init();
    hash.Update(text.data(), text.size());
    hash.Final(digest);
    return looksync::Hex16(digest);
}

static std::string Claim(const std::string& race, const std::string& rider,
                         const std::string& bike, const std::string& skin = "") {
    return "{\"raceNumber\":" + race + ",\"rider\":\"" + rider +
           "\",\"bike\":\"" + bike + "\",\"skin\":\"" + skin + "\"}";
}

int main() {
    assert(Hash("") == "e3b0c44298fc1c149afbf4c8996fb924");
    assert(Hash("abc") == "ba7816bf8f01cfea414140de5dae2223");
    assert(Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039");
    assert(Hash(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67");

    looksync::Look look{7, "A \"{rider}\"", "Bike-1", Hash("abc")};
    looksync::Look parsed;
    assert(looksync::ValidLook(look));
    assert(looksync::ParseClaimJson(looksync::BuildClaimJson(look), parsed));
    assert(parsed.rider == look.rider && parsed.skin == look.skin);
    assert(!looksync::ParseClaimJson("{}", parsed));
    assert(!looksync::ParseClaimJson(Claim("-1", "x", "b"), parsed));
    assert(!looksync::ParseClaimJson(Claim("2147483648", "x", "b"), parsed));
    assert(!looksync::ParseClaimJson(Claim("1.5", "x", "b"), parsed));
    assert(!looksync::ParseClaimJson(Claim("7", "bad\\nname", "b"), parsed));
    assert(!looksync::ParseClaimJson(Claim("7", "x", "bad\\\\bike"), parsed));
    assert(!looksync::ParseClaimJson(Claim("7", "x", "bad/bike"), parsed));
    assert(!looksync::ParseClaimJson(Claim("7", "x", "C:bike"), parsed));
    assert(!looksync::ParseClaimJson(std::string(1025, 'x'), parsed));

    // The extra part ids round trip, are validated like the skin id, and a claim without them
    // (an older client) still parses.
    looksync::Look parts{7, "P", "Bike-1", Hash("a"), Hash("b"), Hash("c"), Hash("d"), Hash("e")};
    looksync::Look back;
    assert(looksync::ParseClaimJson(looksync::BuildClaimJson(parts), back));
    assert(back.swap == parts.swap && back.riderModel == parts.riderModel &&
           back.helmet == parts.helmet && back.boots == parts.boots);
    parts.helmet = "not hex";
    assert(!looksync::ValidLook(parts));
    assert(looksync::ParseClaimJson(Claim("7", "Old", "b"), back) && back.swap.empty());
    assert(!looksync::ParseClaimJson(
        "{\"raceNumber\":7,\"rider\":\"x\",\"bike\":\"b\",\"skin\":\"\",\"swap\":\"zz\"}", back));

    looksync::Registry registry;
    assert(registry.Claim(1, look, 1) == looksync::Registry::Accepted);
    uint32_t seq = registry.Seq();
    assert(registry.Claim(1, look, 2) == looksync::Registry::Accepted);
    assert(registry.Seq() == seq);
    assert(registry.Claim(2, look, 2) == looksync::Registry::Rejected_Taken);

    looksync::Look housemate{8, "Housemate", "Bike-2", Hash("housemate")};
    assert(registry.Claim(1, housemate, 3) == looksync::Registry::Accepted);
    assert(registry.Size() == 2);
    look.skin.clear();
    assert(registry.Claim(1, look, 4) == looksync::Registry::Accepted);
    assert(registry.Size() == 2); // A rider changing skin replaces its own entry.

    looksync::Look third{9, "Third", "b", ""};
    looksync::Look fourth{10, "Fourth", "b", ""};
    looksync::Look fifth{11, "Fifth", "b", ""};
    assert(registry.Claim(1, third, 5) == looksync::Registry::Accepted);
    assert(registry.Claim(1, fourth, 5) == looksync::Registry::Accepted);
    assert(registry.Claim(1, fifth, 5) == looksync::Registry::Rejected_Full);

    std::vector<looksync::Look> looks;
    assert(looksync::ParseFeedJson(registry.BuildFeedJson(), looks, seq));
    assert(looks.size() == 4 && seq == registry.Seq());
    assert(looksync::ParseFeedJson("{\"seq\":7,\"looks\":[]}", looks, seq));
    assert(looks.empty() && seq == 7);
    assert(!looksync::ParseFeedJson("{\"seq\":1,\"looks\":[{", looks, seq));
    registry.Expire(25);
    assert(registry.Size() == 0);

    looksync::Registry full;
    for (int i = 1; i <= 64; ++i) {
        looksync::Look entry{i, "r" + std::to_string(i), "b", ""};
        assert(full.Claim(static_cast<uint32_t>(i), entry, 1) == looksync::Registry::Accepted);
    }
    looksync::Look extra{99, "last", "b", ""};
    assert(full.Claim(100, extra, 1) == looksync::Registry::Rejected_Full);

    looksync::SkinIndex index;
    index.Set("BIKE", Hash("abc"), "red");
    std::string paint;
    assert(index.Find("bike", Hash("abc"), paint) && paint == "red");
    index.Clear();
    assert(!index.Find("bike", Hash("abc"), paint));

    looksync::AppliedKey key{1, 2};
    assert(!looksync::NeedsApply(key, 1, 2));
    assert(looksync::NeedsApply(key, 1, 3));
    std::cout << "looksync tests passed\n";
}
