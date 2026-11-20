// T4.4: the typed property helpers (tools/properties); converted from the "typed property helpers" block of shell/tests/lights.test.ts and the fieldKind test
// of shell/tests/detailsRefs.test.ts.
#include <doctest.h>

#include "tools/properties.h"

using namespace native;

namespace {

core::EntKey key(const std::string& k, const std::string& type, std::optional<std::string> dflt = std::nullopt, std::optional<double> mn = std::nullopt,
                 std::optional<double> mx = std::nullopt) {
    core::EntKey e;
    e.key = k;
    e.type = type;
    e.name = k;
    e.defaultValue = std::move(dflt);
    e.min = mn;
    e.max = mx;
    return e;
}

core::Entity ent(const std::string& cls, std::initializer_list<std::pair<const std::string, std::string>> kv = {}) {
    core::Entity e;
    e.id = "e";
    e.classname = cls;
    for (const auto& p : kv) e.keyvalues.insert(p);
    return e;
}

core::EntClass classDef(const std::string& name, std::vector<core::EntKey> keys) {
    core::EntClass c;
    c.name = name;
    c.keys = std::move(keys);
    return c;
}

} // namespace

TEST_CASE("properties: colour <-> hex <-> \"r g b\"") {
    const auto c = tools::parseColor1("1 0.5 0.25");
    REQUIRE(c);
    CHECK((*c)[0] == 1);
    CHECK((*c)[1] == 0.5);
    CHECK((*c)[2] == 0.25);
    CHECK_FALSE(tools::parseColor1("1 0.5"));
    CHECK_FALSE(tools::parseColor1("a b c"));
    CHECK(tools::colorToHex({1, 0.5, 0}) == "#ff8000");
    CHECK(tools::colorToHex({2, -1, 0}) == "#ff0000");   // clamped
    const auto h = tools::hexToColor("#ff8000");
    REQUIRE(h);
    CHECK((*h)[1] == doctest::Approx(128.0 / 255));
    CHECK_FALSE(tools::hexToColor("nope"));
    CHECK(tools::formatColor({1, 0.5019607843, 0}) == "1 0.502 0");
    CHECK(tools::formatColor(*tools::hexToColor("#ffffff")) == "1 1 1");
    CHECK(tools::formatColor255({1, 0.5, 0}) == "255 128 0");
}

TEST_CASE("properties: numbers stay inside the .ent range, integers are rounded, junk is rejected") {
    const core::EntKey r = key("radius", "real", "10", 0.1, 1000);
    CHECK(*tools::clampValue(r, 5000) == 1000);
    CHECK(*tools::clampValue(r, -3) == 0.1);
    CHECK(*tools::clampValue(r, 2.5) == 2.5);
    const core::EntKey s = key("style", "integer", "0", 0, 63);
    CHECK(*tools::clampValue(s, 7.6) == 8);
    CHECK(*tools::clampValue(s, 99) == 63);
    CHECK_FALSE(tools::clampValue(s, std::nan("")));
}

TEST_CASE("properties: what is a default, what is checked, which keys are typed") {
    const core::EntKey k = key("brightness", "real", "1");
    CHECK(tools::currentValue(ent("light_point"), k) == "1");
    CHECK(tools::isDefault(ent("light_point"), k));
    CHECK(tools::isDefault(ent("light_point", {{"brightness", "1"}}), k));
    CHECK_FALSE(tools::isDefault(ent("light_point", {{"brightness", "2"}}), k));
    CHECK(tools::isChecked("1"));
    CHECK_FALSE(tools::isChecked("0"));
    CHECK_FALSE(tools::isChecked(""));
    CHECK_FALSE(tools::isChecked("false"));
    CHECK(tools::isChecked("yes"));

    const auto names = [](const std::vector<const core::EntKey*>& v) {
        std::vector<std::string> out;
        for (const auto* p : v) out.push_back(p->key);
        return out;
    };
    const core::EntClass two = classDef("x", {key("a", "real", "1"), key("b", "string")});
    CHECK(names(tools::propertyKeys(&two, {})) == std::vector<std::string>{"a", "b"});
    const core::EntClass withAngles = classDef("x", {key("a", "real", "1"), key("angles", "angles", "0 0 0")});
    CHECK(names(tools::propertyKeys(&withAngles, {"angles"})) == std::vector<std::string>{"a"});
    CHECK(tools::propertyKeys(nullptr, {}).empty());
}

TEST_CASE("properties: fieldKind maps .ent types to widgets") {
    using tools::FieldKind;
    CHECK(tools::fieldKind("target", false) == FieldKind::Ref);
    CHECK(tools::fieldKind("target_destination", false) == FieldKind::Ref);
    CHECK(tools::fieldKind("real", false) == FieldKind::Real);
    CHECK(tools::fieldKind("angle", false) == FieldKind::Real);
    CHECK(tools::fieldKind("integer", false) == FieldKind::Integer);
    CHECK(tools::fieldKind("boolean", false) == FieldKind::Boolean);
    CHECK(tools::fieldKind("color", false) == FieldKind::Color01);
    CHECK(tools::fieldKind("color1", false) == FieldKind::Color01);
    CHECK(tools::fieldKind("color255", false) == FieldKind::Color255);
    CHECK(tools::fieldKind("real3", false) == FieldKind::Vec3);
    CHECK(tools::fieldKind("angles", false) == FieldKind::Vec3);
    CHECK(tools::fieldKind("list", true) == FieldKind::List);
    CHECK(tools::fieldKind("string", false) == FieldKind::Text);
    CHECK(tools::fieldKind("model", false) == FieldKind::Asset);
    CHECK(tools::fieldKind("asset", false) == FieldKind::Asset);
    CHECK(tools::fieldKind("array", false) == FieldKind::Text);
}

TEST_CASE("properties: vectors and the scale key") {
    const auto v = tools::parseVec3("1 2.5, 3");
    CHECK(*v[0] == 1);
    CHECK(*v[1] == 2.5);
    CHECK(*v[2] == 3);
    CHECK_FALSE(tools::parseVec3("1 x 3")[1]);
    CHECK_FALSE(tools::parseVec3("")[0]);
    CHECK(tools::formatNumber(0.25) == "0.25");
    CHECK(tools::formatNumber(3) == "3");
    CHECK(tools::formatNumber(-0.0) == "0");
    CHECK(tools::parseScaleVec("2 3 4") == std::array<double, 3>{2, 3, 4});
    CHECK(tools::parseScaleVec("2 0 4") == std::array<double, 3>{1, 1, 1});
    CHECK(tools::parseScaleVec("") == std::array<double, 3>{1, 1, 1});
}
