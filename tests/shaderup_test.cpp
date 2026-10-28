// The upgraded material shaders (src/shaderup.h): the hash table, the source rewrite, the
// settings and the relink rule.
//
// The sources below are written for this test in the shape of the game's (GLSL 1.10, the
// same statements the rules look for); no game shader text is used. Whether every real
// source compiles and links, original and upgraded, is tools/shaderlab's job, since that
// needs a GPU and the exe.
//
// Pure, so it builds and runs anywhere.

#include "../src/shaderup.h"

#include <cstdio>
#include <cstring>
#include <set>
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

using namespace shaderup;

static bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static int Count(const std::string& s, const char* what) {
    int n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}

// A lit, bumped, shadowed, reflective material in the game's shape.
static const char* kFull =
    "#version 110\n"
    "varying vec3 LightDir;varying vec3 EyeDir;uniform sampler2D tex;uniform sampler2D bumpMap;"
    "uniform sampler2D shadowMap;uniform samplerCube cubeMap;uniform float RefFactor;"
    "uniform float RefFactorMin;uniform float FresExp;"
    "void main(){vec3 LightDirNorm;vec4 BumpColor;vec3 BumpNorm;vec3 EyeDirNorm;float edn;float fres;"
    "float specular;vec4 color;vec4 ShadowColor;"
    "BumpColor = texture2D(bumpMap, gl_TexCoord[0].st);"
    "BumpNorm = (BumpColor.xyz - 0.5) * 2.0;"
    "BumpNorm = normalize(BumpNorm);"
    "LightDirNorm = normalize(LightDir);EyeDirNorm = normalize(EyeDir);"
    "specular = pow( max( dot(reflect(LightDirNorm, BumpNorm), EyeDirNorm) , 0.0), gl_FrontMaterial.shininess );"
    "ShadowColor = 1.0 - texture2D(shadowMap, gl_TexCoord[1].st / gl_TexCoord[1].z);"
    "edn = dot(EyeDirNorm,BumpNorm);"
    "fres = RefFactorMin + (RefFactor - RefFactorMin) * pow((1.0 - abs(edn)), FresExp);"
    "color = texture2D(tex, gl_TexCoord[0].st) * ShadowColor + textureCube(cubeMap, EyeDirNorm) * fres + specular;"
    "gl_FragColor = color;}";

// Unbumped, the eye normalised inline.
static const char* kPlain =
    "#version 110\n"
    "varying vec3 LightDir;varying vec3 EyeDir;varying vec3 Normal;"
    "void main(){vec3 LightDirNorm;vec3 Norm;float specular;vec4 color;"
    "LightDirNorm = normalize(LightDir);Norm = normalize(Normal);"
    "specular = pow( max( dot(reflect(LightDirNorm, Norm), normalize(EyeDir)) , 0.0), gl_FrontMaterial.shininess );"
    "color = vec4(specular);"
    "gl_FragColor = color;}";

// Nothing the rules know.
static const char* kSky =
    "#version 110\nuniform sampler2D tex;void main(){gl_FragColor = texture2D(tex, gl_TexCoord[0].st);}";

static void TestHash() {
    // FNV-1a 64 reference vectors.
    CHECK(Fnv1a64("", 0) == 0xcbf29ce484222325ull, "empty");
    CHECK(Fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull, "a");
    CHECK(Fnv1a64("foobar", 6) == 0x85944171f73967e8ull, "foobar");
    CHECK(Hash(std::string(kFull)) != Hash(std::string(kFull) + " "), "one byte changes the hash");
}

static void TestTable() {
    size_t n = 0;
    const Known* k = KnownTable(&n);
    CHECK(n > 0, "the table has rows");
    std::set<uint64_t> hashes;
    for (size_t i = 0; i < n; ++i) {
        CHECK(k[i].hash != 0 && k[i].len > 0, "row %zu is real", i);
        CHECK(hashes.insert(k[i].hash).second, "row %zu: hash appears once", i);
        CHECK(k[i].rules != 0 && (k[i].rules & ~kAllRules) == 0, "row %zu: rules are known bits", i);
        CHECK(k[i].rules & kSpec, "row %zu: every row is a lit material (has the specular rule)", i);
        CHECK(!(k[i].rules & kTone) || (k[i].rules & kSpec), "row %zu: the shoulder only with the specular", i);
        CHECK(k[i].label && k[i].label[0], "row %zu: labelled", i);
        CHECK(Find(k[i].hash, k[i].len) == &k[i], "row %zu: Find finds it", i);
        CHECK(Find(k[i].hash, k[i].len + 1) == nullptr, "row %zu: the length is part of the key", i);
    }
    CHECK(Find(Hash(std::string(kSky)), std::strlen(kSky)) == nullptr, "an unknown source is not found");
    CHECK(Find(0, 0) == nullptr, "zero is not a row");
}

static void TestProbe() {
    CHECK(Probe(kFull) == (kBump | kSpec | kFresnel | kShadow | kTone), "full: every rule (got 0x%x)", Probe(kFull));
    CHECK(Probe(kPlain) == (kSpec | kTone), "plain: spec + shoulder (got 0x%x)", Probe(kPlain));
    CHECK(Probe(kSky) == 0, "sky: nothing");
}

static void TestApply() {
    Config c; c.on = true;
    const std::string full = kFull;

    // Every rule, default strengths.
    Result r = Apply(full, kBump | kSpec | kFresnel | kShadow, c);
    CHECK(r.ok, "full applies (%s)", r.why);
    CHECK(r.applied == (kBump | kSpec | kFresnel | kShadow), "applied mask");
    CHECK(r.out.compare(0, 13, "#version 110\n") == 0, "the #version line stays first");
    CHECK(Has(r.out, "#define FM_BUMP 1.00\n") && Has(r.out, "#define FM_SPEC 1.00\n") &&
          Has(r.out, "#define FM_FRESNEL 0.80\n") && Has(r.out, "#define FM_SOFT 1.00\n"), "defines");
    CHECK(Has(r.out, "BumpNorm = fm_bump(BumpNorm);") && !Has(r.out, "normalize(BumpNorm)"), "bump rewritten");
    CHECK(Has(r.out, "specular = fm_spec(LightDirNorm, BumpNorm, EyeDirNorm, gl_FrontMaterial.shininess);"),
          "spec rewritten with the shader's own normal and eye");
    CHECK(!Has(r.out, "reflect(LightDirNorm"), "no Phong left");
    CHECK(Has(r.out, "fres = fm_fresnel(RefFactorMin, RefFactor, FresExp, edn);"), "fresnel rewritten");
    CHECK(Has(r.out, "1.0 - fm_shadow(shadowMap, gl_TexCoord[1].st / gl_TexCoord[1].z)"), "shadow rewritten");
    CHECK(Count(r.out, "texture2D(shadowMap") == 0, "no single-tap shadow read left in main");
    CHECK(Has(r.out, "gl_FragColor = color;") && !Has(r.out, "fm_tone("), "no shoulder unless asked");
    CHECK(Has(r.out, "float fm_spec(") && Has(r.out, "vec3 fm_bump(") && Has(r.out, "float fm_fresnel(") &&
          Has(r.out, "vec4 fm_shadow("), "the helpers that are used are defined");
    // The helpers come before main, after #version.
    CHECK(r.out.find("fm_spec(vec3") < r.out.find("void main"), "helpers precede main");
    // Everything the game declared is still there, untouched.
    CHECK(Has(r.out, "uniform float FresExp;") && Has(r.out, "varying vec3 EyeDir;"), "declarations kept");

    // The shoulder.
    r = Apply(full, kSpec | kTone, c);
    CHECK(r.ok && Has(r.out, "gl_FragColor = fm_tone(color);") && Has(r.out, "vec4 fm_tone("), "tone");
    CHECK(!Has(r.out, "vec3 fm_bump("), "an unused helper is not emitted");

    // The inline normalize(EyeDir) form.
    r = Apply(kPlain, kSpec, c);
    CHECK(r.ok && Has(r.out, "specular = fm_spec(LightDirNorm, Norm, normalize(EyeDir), gl_FrontMaterial.shininess);"),
          "plain spec (%s)", r.why);

    // A rule the source cannot take refuses the whole source.
    r = Apply(kPlain, kSpec | kBump, c);
    CHECK(!r.ok && r.out.empty(), "plain has no bump: refused");
    r = Apply(kSky, kSpec, c);
    CHECK(!r.ok, "sky: refused");
    r = Apply(full, 0, c);
    CHECK(!r.ok, "nothing asked: refused");

    // Strengths: locale-free fixed point.
    Config s = c; s.bump = 150; s.spec = 5; s.fresnel = 0; s.soft = 250;
    r = Apply(full, kBump | kSpec, s);
    CHECK(r.ok && Has(r.out, "#define FM_BUMP 1.50\n") && Has(r.out, "#define FM_SPEC 0.05\n") &&
          Has(r.out, "#define FM_SOFT 2.50\n") && Has(r.out, "#define FM_FRESNEL 0.00\n"), "strength defines");
    CHECK(Fixed(0) == "0.00" && Fixed(7) == "0.07" && Fixed(100) == "1.00" && Fixed(300) == "3.00" &&
          Fixed(-25) == "-0.25", "Fixed");

    // An upgraded source is never upgraded again.
    Result twice = Apply(Apply(full, kSpec, c).out, kSpec, c);
    CHECK(!twice.ok, "already upgraded: refused");

    // Malformed specular lines are refused, not half-rewritten.
    std::string bad = full;
    bad.replace(bad.find(") , 0.0), gl_FrontMaterial.shininess );"), 4, ")) , ");
    r = Apply(bad, kSpec, c);
    CHECK(!r.ok, "unbalanced spec line refused");
    bad = "#version 110\nvoid main(){specular = pow( max( dot(reflect(LightDirNorm, a+b), E) , 0.0), gl_FrontMaterial.shininess );}";
    CHECK(!Apply(bad, kSpec, c).ok, "an expression as the normal is refused");

    // No #version line, or not the first thing: refused.
    CHECK(!Apply(std::string(" ") + kFull, kSpec, c).ok, "leading space before #version");
}

static void TestUpgrade() {
    Config c; c.on = true;
    const std::string full = kFull;
    Known k{Hash(full), (uint32_t)full.size(), kBump | kSpec | kFresnel | kShadow | kTone, "test"};

    Result r = Upgrade(full, k, c);
    CHECK(r.ok, "upgrade ok (%s)", r.why);
    CHECK(r.applied == (kBump | kSpec | kFresnel | kShadow), "tone is off by default (0x%x)", r.applied);

    c.tone = true;
    CHECK(Upgrade(full, k, c).applied == k.rules, "every rule with tone on");
    c.tone = false;

    c.fresnel = 0; c.soft = 0;
    r = Upgrade(full, k, c);
    CHECK(r.ok && r.applied == (kBump | kSpec), "fresnel=0 and soft=0 leave those parts as the game wrote them");
    CHECK(Has(r.out, "texture2D(shadowMap, gl_TexCoord[1].st") && Has(r.out, "pow((1.0 - abs(edn)), FresExp)"),
          "untouched parts are the original text");

    // A source with the right hash but a row that promises a rule it cannot take: refused.
    Known lie{Hash(std::string(kPlain)), (uint32_t)std::strlen(kPlain), kSpec | kBump, "lie"};
    CHECK(!Upgrade(kPlain, lie, Config{}).ok, "a row that promises bump on an unbumped source is refused");

    // Hash or length off by anything: refused.
    Known wrong = k; wrong.hash ^= 1;
    CHECK(!Upgrade(full, wrong, c).ok, "hash mismatch refused");
    wrong = k; wrong.len += 1;
    CHECK(!Upgrade(full, wrong, c).ok, "length mismatch refused");
}

static void TestConfig() {
    Config c;
    CHECK(!c.on && !c.grade && !c.tone, "everything off by default");
    CHECK(CfgLines(c).empty(), "defaults write nothing");
    CHECK(ParseCfgLine("shaders=1\n", c) && c.on, "shaders=1");
    CHECK(ParseCfgLine("shaders=2\n", c) && !c.on, "only exactly 1 turns it on");
    CHECK(ParseCfgLine("shaderbump=150", c) && c.bump == 150, "bump");
    CHECK(ParseCfgLine("shaderbump=9999", c) && c.bump == 300, "bump clamped");
    CHECK(ParseCfgLine("shaderspec=-5", c) && c.spec == 0, "spec clamped low");
    CHECK(ParseCfgLine("shaderfresnel=abc", c) && c.fresnel == 80, "junk keeps the default");
    CHECK(ParseCfgLine("gradebright=10", c) && c.bright == 50, "brightness clamped");
    CHECK(ParseCfgLine("grade=1", c) && c.grade, "grade");
    CHECK(ParseCfgLine("gradesat=120", c) && c.sat == 120, "saturation");
    CHECK(!ParseCfgLine("mapguard=0", c), "not ours");
    CHECK(!ParseCfgLine("shader=1", c), "a near miss is not ours");

    Config a; a.on = true; a.bump = 140; a.spec = 80; a.fresnel = 50; a.soft = 0; a.tone = true;
    a.grade = true; a.bright = 105; a.contrast = 110; a.sat = 90;
    Config b;
    std::string lines = CfgLines(a);
    for (size_t p = 0, e; (e = lines.find('\n', p)) != std::string::npos; p = e + 1)
        CHECK(ParseCfgLine(lines.substr(p, e - p).c_str(), b), "round trip line");
    CHECK(b.on == a.on && b.bump == a.bump && b.spec == a.spec && b.fresnel == a.fresnel && b.soft == a.soft &&
          b.tone == a.tone && b.grade == a.grade && b.bright == a.bright && b.contrast == a.contrast &&
          b.sat == a.sat, "write then read gives the same settings");

    Config e; e.on = true;
    CHECK(EnabledRules(e) == (kBump | kSpec | kFresnel | kShadow), "default rules");
    e.fresnel = 0; e.soft = 0; e.tone = true;
    CHECK(EnabledRules(e) == (kBump | kSpec | kTone), "turned-down rules drop out");
    CHECK(GradeIsIdentity(Config{}), "the default grade changes nothing");
}

static void TestLocations() {
    std::vector<Loc> game = {{"tex", 0}, {"bumpMap", 1}, {"FresExp", 2}, {"RefFactor", 3}};
    std::string why;
    CHECK(LocationsCompatible(game, game), "same link");
    CHECK(LocationsCompatible(game, {{"tex", 0}, {"bumpMap", 1}, {"RefFactor", 3}}),
          "a dropped uniform is fine when its slot stays empty");
    CHECK(!LocationsCompatible(game, {{"tex", 0}, {"bumpMap", 1}, {"RefFactor", 2}}, &why),
          "a uniform that moved is refused");
    CHECK(!why.empty(), "and says why");
    CHECK(!LocationsCompatible({{"tex", 0}, {"RefFactor", 3}}, {{"tex", 0}, {"FresExp", 3}, {"RefFactor", 4}}, &why),
          "a new uniform in the game's slot is refused");
    CHECK(LocationsCompatible({{"tex", 0}, {"RefFactor", 3}}, {{"tex", 0}, {"FresExp", 7}, {"RefFactor", 3}}),
          "a new uniform in a free slot is fine");
    CHECK(LocationsCompatible({}, {}), "nothing to keep");
}

static void TestUniformTypes() {
    UType u{};
    CHECK(UniformType(0x1406, &u) && u.comps == 1 && !u.isInt, "float");
    CHECK(UniformType(0x8B51, &u) && u.comps == 3 && !u.isInt, "vec3");
    CHECK(UniformType(0x8B5C, &u) && u.comps == 16 && u.isMatrix, "mat4");
    CHECK(UniformType(0x8B5B, &u) && u.comps == 9 && u.isMatrix, "mat3");
    CHECK(UniformType(0x8B5E, &u) && u.isInt && u.comps == 1, "sampler2D is an int");
    CHECK(UniformType(0x8B60, &u) && u.isInt, "samplerCube is an int");
    CHECK(!UniformType(0x1234, &u), "unknown type");
}

static void TestGrade() {
    const std::string g = GradeFragmentSource();
    CHECK(g.compare(0, 13, "#version 110\n") == 0, "grade is GLSL 1.10 like the game");
    CHECK(Has(g, "uniform sampler2D fm_frame;") && Has(g, "uniform vec3 fm_grade;"), "grade uniforms");
}

int main() {
    TestHash();
    TestTable();
    TestProbe();
    TestApply();
    TestUpgrade();
    TestConfig();
    TestLocations();
    TestUniformTypes();
    TestGrade();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("shaderup: all checks passed\n");
    return 0;
}
