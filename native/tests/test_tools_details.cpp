// T4.4: the Details panel model and the edits behind it (tools/details, tools/entity_edit, tools/ref_targets); converted from shell/tests/detailsRefs.test.ts and the
// matching parts of the Shell's actions tests.
#include <doctest.h>

#include "core/entdef.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "tools/details.h"
#include "tools/entity_edit.h"

using namespace native;
using doc::SelRef;
using tools::DetailsEdit;
using tools::DetailsRow;
using tools::EditValue;
using tools::RowKind;

namespace {

using DocPtr = std::shared_ptr<const doc::DocumentVersion>;

DocPtr emptyDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    return r.version;
}

// Makes the result of an edit the current document (and fails the test when it was refused).
DocPtr take(const DocPtr& base, const tools::EditResult& r) {
    INFO(r.error);
    REQUIRE(r.ok());
    REQUIRE(r.version != base);
    return r.version;
}

std::string spawn(DocPtr& d, const std::string& name, double x, const std::string& cls = "info_x") {
    const auto r = tools::addPointEntity(d, cls, {x, 0, 0}, {{"targetname", name}});
    d = take(d, r);
    REQUIRE(r.select.size() == 1);
    return r.select[0].entityId;
}

const core::Entity& entityOf(const DocPtr& d, const std::string& id) {
    const int i = d->findEntity(id);
    REQUIRE(i >= 0);
    return d->entities[static_cast<size_t>(i)]->typed;
}

tools::DetailsForm formFor(const DocPtr& d, const std::string& entityId, const core::EntityDefinitions* defs = nullptr, std::set<std::string> pending = {}) {
    const SelRef sel = SelRef::entity(entityId);
    tools::DetailsContext ctx;
    ctx.doc = d;
    ctx.selection = &sel;
    ctx.defs = defs;
    ctx.pendingRefs = std::move(pending);
    return tools::buildDetails(ctx);
}

std::vector<std::string> labels(const tools::DetailsForm& f) {
    std::vector<std::string> out;
    for (const DetailsRow& r : f.rows)
        if (r.kind != RowKind::Section && r.kind != RowKind::Note && r.kind != RowKind::AddKey) out.push_back(r.label);
    return out;
}

const DetailsRow* rowOf(const tools::DetailsForm& f, const std::string& label) {
    for (const DetailsRow& r : f.rows)
        if (r.label == label && r.kind != RowKind::Section) return &r;
    return nullptr;
}

std::vector<std::string> refLabels(const tools::DetailsForm& f) {
    std::vector<std::string> out;
    for (const DetailsRow& r : f.rows)
        if (r.kind == RowKind::Ref) out.push_back(r.label);
    return out;
}

const core::EntityDefinitions& navDefs() {
    static const core::EntityDefinitions d = core::parseEntityDefinitions(R"(
<point name="nav_point" color="0 0.5 0" box="-8 -8 0 8 8 32">
<angle key="angle" name="Yaw Angle" default="0">a</angle>
<string key="targetname" name="Name">n</string>
<real key="waitTime" name="Wait Time" default="0">w</real>
<target key="target" name="Next nav point">t</target>
<boolean key="loop" name="Loop">l</boolean>
<integer key="count" name="Count">c</integer>
<real3 key="targetLocation" name="Target Location" default="0 0 0">p</real3>
<string key="classname" name="Classname" default="nav_point">x</string>
</point>)");
    return d;
}

DetailsEdit editOf(const tools::DetailsForm& f, const std::string& label) {
    const DetailsRow* r = rowOf(f, label);
    REQUIRE(r);
    return r->edit;
}

} // namespace

TEST_CASE("details: a reference with several targets shows its list; picking and removing edit it") {
    DocPtr d = emptyDoc();
    const std::string a = spawn(d, "a", 0), b = spawn(d, "b", 1), c = spawn(d, "c", 2), e = spawn(d, "d", 3);
    d = take(d, tools::setRef(d, c, "target", {a, b}));

    const tools::DetailsForm f = formFor(d, c);
    REQUIRE(f.mode == tools::DetailsForm::Mode::Entity);
    const DetailsRow* ref = rowOf(f, "target");
    REQUIRE(ref);
    CHECK(ref->kind == RowKind::Ref);
    CHECK(ref->paths == std::vector<std::string>{a, b});
    CHECK_FALSE(ref->warning.empty());   // the names a and b differ
    CHECK(ref->canShare);
    CHECK(ref->excludePath == c);        // an entity is not its own target
    CHECK(f.targets.size() == 4);

    EditValue add;
    add.paths = {a, b, e};
    d = take(d, tools::applyDetailsEdit(d, nullptr, ref->edit, add));
    CHECK(entityOf(d, c).refs.at("target").paths == std::vector<std::string>{a, b, e});

    EditValue removeFirst;
    removeFirst.paths = {b, e};
    d = take(d, tools::applyDetailsEdit(d, nullptr, rowOf(formFor(d, c), "target")->edit, removeFirst));
    CHECK(entityOf(d, c).refs.at("target").paths == std::vector<std::string>{b, e});

    EditValue none;
    d = take(d, tools::applyDetailsEdit(d, nullptr, rowOf(formFor(d, c), "target")->edit, none));
    CHECK(entityOf(d, c).refs.count("target") == 0);
}

TEST_CASE("details: Share first name gives every target the name of the first") {
    DocPtr d = emptyDoc();
    const std::string a = spawn(d, "a", 0), b = spawn(d, "b", 1);
    DetailsEdit share;
    share.kind = DetailsEdit::Kind::ShareName;
    EditValue v;
    v.paths = {a, b};
    d = take(d, tools::applyDetailsEdit(d, nullptr, share, v));
    CHECK(entityOf(d, b).keyvalues.at("targetname") == "a");
    CHECK(tools::applyDetailsEdit(d, nullptr, share, v).version == nullptr);   // nothing left to do
}

TEST_CASE("details: targetname is right after the id; an undefined class gets no target pickers it did not ask for") {
    DocPtr d = emptyDoc();
    const auto r = tools::addPointEntity(d, "info_notnull", {0, 0, 0}, {{"targetname", "door1"}, {"note", "x"}});
    d = take(d, r);
    const std::string id = r.select[0].entityId;

    const tools::DetailsForm f = formFor(d, id);
    const std::vector<std::string> ls = labels(f);
    const auto at = [&](const std::string& l) { return std::find(ls.begin(), ls.end(), l) - ls.begin(); };
    CHECK(at("targetname") == at("id") + 1);
    CHECK(std::count(ls.begin(), ls.end(), "targetname") == 1);
    CHECK(refLabels(f).empty());
    CHECK(std::find(ls.begin(), ls.end(), "note") != ls.end());
    CHECK(rowOf(f, "targetname")->value == "door1");

    EditValue v;
    v.text = "door2";
    d = take(d, tools::applyDetailsEdit(d, nullptr, editOf(f, "targetname"), v));
    CHECK(entityOf(d, id).keyvalues.at("targetname") == "door2");
    v.text = "  ";   // an empty name removes the key
    d = take(d, tools::applyDetailsEdit(d, nullptr, editOf(formFor(d, id), "targetname"), v));
    CHECK(entityOf(d, id).keyvalues.count("targetname") == 0);

    // a reference that is set shows its picker; adding "killtarget" adds an empty picker row, not a text value
    d = take(d, tools::setRef(d, id, "target", {id}));
    CHECK(refLabels(formFor(d, id)) == std::vector<std::string>{"target"});
    const tools::DetailsForm withPending = formFor(d, id, nullptr, {"killtarget"});
    CHECK(refLabels(withPending) == std::vector<std::string>{"target", "killtarget"});
    CHECK(entityOf(d, id).keyvalues.count("killtarget") == 0);
    CHECK(std::find(f.refKeys.begin(), f.refKeys.end(), "killtarget") != f.refKeys.end());
    CHECK(std::find(withPending.refKeys.begin(), withPending.refKeys.end(), "killtarget") == withPending.refKeys.end());
}

TEST_CASE("details: a defined class gives each key the widget of its type, in .ent order, the target picker among them") {
    DocPtr d = emptyDoc();
    const auto r = tools::addPointEntity(d, "nav_point", {0, 0, 0});
    d = take(d, r);
    const std::string id = r.select[0].entityId;
    const core::EntityDefinitions& defs = navDefs();
    REQUIRE(defs.find("nav_point"));

    const tools::DetailsForm f = formFor(d, id, &defs);
    // angle -> the angles row, targetname -> the top, classname -> the classname field: the typed section lists only the rest
    std::vector<std::string> typed;
    bool in = false;
    for (const DetailsRow& row : f.rows) {
        if (row.kind == RowKind::Section) in = row.label == "Properties";
        else if (in) typed.push_back(row.label);
    }
    CHECK(typed == std::vector<std::string>{"Wait Time", "Next nav point", "Loop", "Count", "Target Location"});
    CHECK(rowOf(f, "Next nav point")->kind == RowKind::Ref);
    CHECK(rowOf(f, "Loop")->kind == RowKind::Check);
    CHECK(rowOf(f, "Wait Time")->kind == RowKind::Real);
    CHECK(rowOf(f, "Wait Time")->value == "0");
    CHECK(rowOf(f, "Count")->kind == RowKind::Integer);
    CHECK(*rowOf(f, "Count")->step == 1);
    CHECK(rowOf(f, "Target Location")->kind == RowKind::Vec3);
    CHECK(rowOf(f, "Target Location")->vec[2] == "0");

    EditValue v;
    v.number = 0.25;
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "Wait Time"), v));
    CHECK(entityOf(d, id).keyvalues.at("waitTime") == "0.25");
    v.number = 2.6;
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "Count"), v));
    CHECK(entityOf(d, id).keyvalues.at("count") == "3");
    DetailsEdit loc = editOf(f, "Target Location");
    loc.axis = 2;
    v.number = 5;
    d = take(d, tools::applyDetailsEdit(d, &defs, loc, v));
    CHECK(entityOf(d, id).keyvalues.at("targetLocation") == "0 0 5");
    v.checked = true;
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "Loop"), v));
    CHECK(entityOf(d, id).keyvalues.at("loop") == "1");
    CHECK(rowOf(formFor(d, id, &defs), "Loop")->checked);
    DetailsEdit reset = rowOf(formFor(d, id, &defs), "Loop")->reset;
    CHECK(rowOf(formFor(d, id, &defs), "Loop")->resettable);
    d = take(d, tools::applyDetailsEdit(d, &defs, reset, EditValue{}));
    CHECK(entityOf(d, id).keyvalues.count("loop") == 0);
}

TEST_CASE("details: colours, lists and ranges of typed keys") {
    const core::EntityDefinitions defs = core::parseEntityDefinitions(R"(
<point name="light_point" color="1 1 0" box="-8 -8 -8 8 8 8">
<color1 key="_color" name="Colour" default="1 1 1">c</color1>
<color255 key="rgb" name="RGB">c</color255>
<real key="radius" name="Radius" default="10" min="0.1" max="1000">r</real>
<list key="style" name="Style"><item name="Normal" value="0"/><item name="Flicker" value="1"/></list>
</point>)");
    DocPtr d = emptyDoc();
    const auto r = tools::addPointEntity(d, "light_point", {0, 0, 0});
    d = take(d, r);
    const std::string id = r.select[0].entityId;
    tools::DetailsForm f = formFor(d, id, &defs);
    CHECK(f.rows.size() > 5);
    CHECK(rowOf(f, "Colour")->kind == RowKind::Color);
    CHECK(rowOf(f, "Colour")->vec[0] == "#ffffff");
    CHECK(rowOf(f, "Style")->kind == RowKind::Choice);
    CHECK(rowOf(f, "Style")->choices.size() == 3);   // the two items and the unset "—"

    EditValue v;
    v.text = "#ff8000";
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "Colour"), v));
    CHECK(entityOf(d, id).keyvalues.at("_color") == "1 0.502 0");
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "RGB"), v));
    CHECK(entityOf(d, id).keyvalues.at("rgb") == "255 128 0");
    v.number = 5000;
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "Radius"), v));
    CHECK(entityOf(d, id).keyvalues.at("radius") == "1000");   // clamped to the .ent range
    v.text = "1";
    d = take(d, tools::applyDetailsEdit(d, &defs, editOf(f, "Style"), v));
    CHECK(entityOf(d, id).keyvalues.at("style") == "1");
    f = formFor(d, id, &defs);
    CHECK(rowOf(f, "Style")->value == "1");
    CHECK(rowOf(f, "Style")->choices.size() == 2);
    CHECK(rowOf(f, "Radius")->changed);
}

TEST_CASE("entity edits: unchanged values change nothing; angles, scale, classname, spawnflags") {
    DocPtr d = emptyDoc();
    const std::string id = spawn(d, "x", 1);
    CHECK_FALSE(tools::setKeyvalue(d, id, "targetname", "x").ok());
    CHECK_FALSE(tools::setOrigin(d, id, 0, 1).ok());
    CHECK_FALSE(tools::setKeyvalue(d, id, "nothing", std::nullopt).ok());
    d = take(d, tools::setOrigin(d, id, 1, 4));
    CHECK(entityOf(d, id).origin.y == 4);
    d = take(d, tools::setAngle(d, id, 0, 45));
    CHECK(entityOf(d, id).angles.x == 45);
    // scale is the modelscale_vec key; 1 1 1 removes it
    d = take(d, tools::setScale(d, id, 2, 2));
    CHECK(entityOf(d, id).keyvalues.at("modelscale_vec") == "1 1 2");
    CHECK(tools::setScale(d, id, 0, 0).error.find("cannot be 0") != std::string::npos);
    d = take(d, tools::setScale(d, id, 2, 1));
    CHECK(entityOf(d, id).keyvalues.count("modelscale_vec") == 0);
    d = take(d, tools::setClassname(d, id, "info_y"));
    CHECK(entityOf(d, id).classname == "info_y");
    CHECK_FALSE(tools::setClassname(d, id, "").ok());

    DetailsEdit flag;
    flag.kind = DetailsEdit::Kind::Spawnflag;
    flag.entityId = id;
    flag.bit = 2;
    EditValue on;
    on.checked = true;
    d = take(d, tools::applyDetailsEdit(d, nullptr, flag, on));
    CHECK(entityOf(d, id).keyvalues.at("spawnflags") == "4");
    flag.bit = 0;
    d = take(d, tools::applyDetailsEdit(d, nullptr, flag, on));
    CHECK(entityOf(d, id).keyvalues.at("spawnflags") == "5");
    flag.bit = 2;
    d = take(d, tools::applyDetailsEdit(d, nullptr, flag, EditValue{}));
    CHECK(entityOf(d, id).keyvalues.at("spawnflags") == "1");
    flag.bit = 0;
    d = take(d, tools::applyDetailsEdit(d, nullptr, flag, EditValue{}));
    CHECK(entityOf(d, id).keyvalues.count("spawnflags") == 0);
}

TEST_CASE("entity edits: new lights point down; a brush's operation and carve flags") {
    DocPtr d = emptyDoc();
    const auto spot = tools::addPointEntity(d, "light_spot", {0, 0, 3});
    d = take(d, spot);
    CHECK(entityOf(d, spot.select[0].entityId).angles.x == 90);
    CHECK(tools::defaultLightAngles("light_point") == std::nullopt);
    CHECK(tools::defaultLightAngles("light_directional").has_value());

    doc::VersionEditor ed(d);
    core::Entity g;
    g.id = "g";
    g.classname = "func_group";
    g.brushes.push_back(core::Brush::makeBox("g1", {0, 0, 0}, {1, 1, 1}, "dev/grey"));
    g.brushes.push_back(core::Brush::makeBox("g2", {2, 0, 0}, {3, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(g));
    d = ed.finish();
    d = take(d, tools::setBrushOp(d, "g", "g1", true));
    CHECK(d->entities[static_cast<size_t>(d->findEntity("g"))]->brushes[0]->brush.op == core::BrushOp::Subtract);
    CHECK_FALSE(tools::setBrushOp(d, "g", "g1", true).ok());
    d = take(d, tools::setCarve(d, "g", "g1", tools::CarveKey::NonStructural, true));
    CHECK(d->entities[static_cast<size_t>(d->findEntity("g"))]->brushes[0]->brush.carve.nonStructural);

    const SelRef sel = SelRef::brush("g", "g1");
    tools::DetailsContext ctx;
    ctx.doc = d;
    ctx.selection = &sel;
    const tools::DetailsForm f = tools::buildDetails(ctx);
    CHECK(rowOf(f, "operation")->value == "subtract");
    CHECK(rowOf(f, "other non-structural entities")->checked);
    CHECK(rowOf(f, "its own entity")->checked);
    CHECK(rowOf(f, "Vertex lit only"));
    CHECK(rowOf(f, "origin") == nullptr);   // a brush entity has no origin row
}

TEST_CASE("details: a prefab instance shows its name, transform, contents and overrides; edits go through the override family") {
    DocPtr d = emptyDoc();
    const auto spawned = tools::spawnInstance(d, "prefabs/door.map.json", {1, 2, 3});
    d = take(d, spawned);
    const std::string inst = spawned.select[0].instancePath[0];
    CHECK(d->instances[0]->typed.name == "door");

    // names: empty, with '|' or '/', or used by another instance are refused
    CHECK_FALSE(tools::renameInstance(d, inst, "  ").ok());
    CHECK(tools::renameInstance(d, inst, "a|b").error.find("'|'") != std::string::npos);
    d = take(d, tools::renameInstance(d, inst, "front"));
    const auto second = tools::spawnInstance(d, "prefabs/door.map.json", {0, 0, 0});
    d = take(d, second);
    CHECK(tools::renameInstance(d, second.select[0].instancePath[0], "front").error.find("already") != std::string::npos);

    d = take(d, tools::setInstanceVec(d, inst, tools::InstanceVec::Angles, 1, 90));
    CHECK(d->instances[0]->typed.angles.y == 90);
    CHECK(tools::setInstanceVec(d, inst, tools::InstanceVec::Scale, 0, 0).error.find("cannot be 0") != std::string::npos);
    d = take(d, tools::setInstanceVec(d, inst, tools::InstanceVec::Scale, 0, 2));
    REQUIRE(d->instances[0]->typed.scale);
    CHECK(d->instances[0]->typed.scale->x == 2);
    CHECK(d->instances[0]->typed.scale->y == 1);

    // the prefab's content is given to the model (the expansion supplies it in the app)
    core::ResolvedEntity r;
    r.entity.id = "hinge";
    r.entity.classname = "light";
    r.entity.keyvalues["targetname"] = "h";
    r.instancePath = {"root", inst};
    r.sourceEntityId = "hinge";
    const std::vector<core::ResolvedEntity> content = {r};

    const SelRef sel = SelRef::instance(inst);
    tools::DetailsContext ctx;
    ctx.doc = d;
    ctx.selection = &sel;
    ctx.prefab = &content;
    ctx.openPath = "hinge";
    tools::DetailsForm f = tools::buildDetails(ctx);
    REQUIRE(f.mode == tools::DetailsForm::Mode::Instance);
    CHECK(rowOf(f, "name")->value == "front");
    CHECK(rowOf(f, "h")->kind == RowKind::Content);
    CHECK(rowOf(f, "h")->open);
    const DetailsRow* kv = rowOf(f, "targetname");
    REQUIRE(kv);
    CHECK_FALSE(kv->resettable);

    EditValue v;
    v.text = "h2";
    d = take(d, tools::applyDetailsEdit(d, nullptr, kv->edit, v));
    CHECK(d->instances[0]->typed.overrides.at("hinge").keyvalues.at("targetname") == "h2");
    ctx.doc = d;
    f = tools::buildDetails(ctx);
    CHECK(rowOf(f, "targetname")->resettable);
    CHECK(rowOf(f, "targetname")->changed);
    bool summary = false;
    for (const DetailsRow& row : f.rows) summary = summary || (row.kind == RowKind::Summary && row.label == "hinge" && row.value == "targetname");
    CHECK(summary);

    // reverting the only override removes the (now empty) override
    DetailsEdit revert = rowOf(f, "targetname")->reset;
    d = take(d, tools::applyDetailsEdit(d, nullptr, revert, EditValue{}));
    CHECK(d->instances[0]->typed.overrides.empty());

    // deleting an entity in the instance, then restoring it
    DetailsEdit del;
    del.kind = DetailsEdit::Kind::OverrideDelete;
    del.instanceId = inst;
    del.path = "hinge";
    d = take(d, tools::applyDetailsEdit(d, nullptr, del, EditValue{}));
    CHECK(d->instances[0]->typed.overrides.at("hinge").deleted);
    DetailsEdit restore;
    restore.kind = DetailsEdit::Kind::OverrideRevert;
    restore.instanceId = inst;
    restore.path = "hinge";
    d = take(d, tools::applyDetailsEdit(d, nullptr, restore, EditValue{}));
    CHECK(d->instances[0]->typed.overrides.empty());
}

TEST_CASE("details: nothing selected and refs targets of prefab content") {
    DocPtr d = emptyDoc();
    tools::DetailsContext ctx;
    ctx.doc = d;
    CHECK(tools::buildDetails(ctx).mode == tools::DetailsForm::Mode::Nothing);
    CHECK(tools::buildDetails(ctx).message == "Nothing selected.");

    const std::string a = spawn(d, "a", 0);
    const auto inst = tools::spawnInstance(d, "p.json", {0, 0, 0});
    d = take(d, inst);
    core::ResolvedEntity r;
    r.entity.id = "x";
    r.entity.classname = "light";
    r.entity.keyvalues["targetname"] = "lamp";
    r.instancePath = {"root", inst.select[0].instancePath[0]};
    r.sourceEntityId = "x";
    const auto targets = tools::refTargets(*d, {r});
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].path == a);
    CHECK(targets[0].group.empty());
    CHECK(targets[1].path == inst.select[0].instancePath[0] + "/x");
    CHECK(targets[1].group == "p");
    CHECK(targets[1].resolvedName == "p|lamp");
    CHECK(targets[1].label == "lamp  (x)");

    const std::vector<std::string> keys = tools::refKeysOf(navDefs().find("nav_point"), {"extra"});
    CHECK(keys == std::vector<std::string>{"target", "killtarget", "extra"});
}
