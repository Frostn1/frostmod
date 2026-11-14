#include "../src/riderlooks.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static std::string Id(char c) {
    return std::string(32, c);
}

static void RoundTrip() {
    frostmod::RiderLook look;
    look.rider = u8" R|i;d:e%r\"\\é ";
    look.gear[0] = Id('a');
    look.gear[1] = Id('b');
    look.gear[2] = Id('c');
    look.bikes.push_back({u8"Bike: |;%\"\\雪", Id('d')});
    const std::string encoded = frostmod::BuildRiderLooks({look});
    assert(encoded.find('|') != std::string::npos);
    assert(encoded.find("%7C") != std::string::npos);
    assert(encoded.find("%3B") != std::string::npos);
    assert(encoded.find("%3A") != std::string::npos);
    assert(encoded.find("%25") != std::string::npos);
    assert(encoded.find("%22") != std::string::npos);
    assert(encoded.find("%5C") != std::string::npos);
    assert(encoded.find("%C3%A9") != std::string::npos);
    assert(encoded.find("%E9%9B%AA") != std::string::npos);
    std::vector<frostmod::RiderLook> parsed;
    int skipped = -1;
    assert(frostmod::ParseRiderLooks(encoded, parsed, &skipped));
    assert(skipped == 0 && parsed.size() == 1);
    assert(parsed[0].rider == look.rider);
    assert(parsed[0].gear[1] == Id('b'));
    assert(parsed[0].bikes == look.bikes);
    assert(frostmod::BikeSwapId(parsed[0], "  bike: |;%\"\\雪  ") == Id('d'));
}

static void EmptyAndNames() {
    std::vector<frostmod::RiderLook> parsed(1);
    int skipped = -1;
    assert(frostmod::ParseRiderLooks("", parsed, &skipped));
    assert(parsed.empty() && skipped == 0);
    assert(frostmod::NameMatches("  Sean Rider\t", "seAN riDER"));
    assert(!frostmod::NameMatches("Sean", "Seán"));
    assert(frostmod::PercentDecode("a%7Cb%3Ac%25") == "a|b:c%");
}

static void LimitsAndBadInput() {
    frostmod::RiderLook look;
    look.rider = "rider";
    look.gear[0] = Id('A');
    look.gear[1] = "not-an-id";
    look.bikes.push_back({"bike", Id('G')});
    const std::string normalized = frostmod::BuildRiderLooks({look});
    std::vector<frostmod::RiderLook> parsed;
    int skipped = 0;
    assert(frostmod::ParseRiderLooks(normalized, parsed, &skipped));
    assert(parsed.size() == 1);
    assert(parsed[0].gear[0].empty() && parsed[0].gear[1].empty());
    assert(parsed[0].bikes[0].second.empty());

    assert(frostmod::ParseRiderLooks("r|xyz|||bike:XYZ", parsed, &skipped));
    assert(parsed.size() == 1 && parsed[0].gear[0].empty());
    assert(parsed[0].bikes[0].second.empty());
    assert(frostmod::ParseRiderLooks("bad;r|||;also%Q0|||", parsed, &skipped));
    assert(parsed.size() == 1 && skipped == 2);

    std::vector<frostmod::RiderLook> many(65);
    for (size_t i = 0; i < many.size(); ++i) {
        many[i].rider = "r" + std::to_string(i);
    }
    const std::string capped = frostmod::BuildRiderLooks(many);
    assert(frostmod::ParseRiderLooks(capped, parsed, &skipped));
    assert(parsed.size() == 64 && skipped == 0);

    std::string tooMany;
    for (int i = 0; i < 65; ++i) {
        if (i) {
            tooMany.push_back(';');
        }
        tooMany += "r" + std::to_string(i) + "|||";
    }
    assert(frostmod::ParseRiderLooks(tooMany, parsed, &skipped));
    assert(parsed.size() == 64 && skipped == 1);

    std::string pairs = "r|||";
    for (int i = 0; i < 17; ++i) {
        pairs += "|b" + std::to_string(i) + ":" + Id('a');
    }
    assert(frostmod::ParseRiderLooks(pairs, parsed, &skipped));
    assert(parsed.empty() && skipped == 1);
}

int main() {
    RoundTrip();
    EmptyAndNames();
    LimitsAndBadInput();
    std::cout << "riderlooks tests passed\n";
    return 0;
}
