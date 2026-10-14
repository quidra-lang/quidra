#include "ir/canonical_encoding.hpp"
#include "ir/content_hash.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct SemanticWithSpan { bool writes{}; SourceSpan site; };

void mutation_and_stitch() {
    const auto integer = Type::simple(TypeKind::Int64);
    Function function;
    function.name = "add";
    function.source_file = "input.qui";
    function.source_line = 3;
    function.source_column = 5;
    function.declaration = DeclarationIdentity{};
    function.declaration->module_path = "example";
    function.declaration->source_file = "input.qui";
    function.declaration->signature = "add(int,int)->int";
    function.blocks.push_back({"entry", {SourceLocation{{3,5},"input.qui","revision","node","binary"},
        ConstantInt{1,"7",integer}, Binary{2,"+",1,1,integer,integer,3,9}, Return{2,integer}}});
    function.next_value = 3;
    const auto before = identify(function);
    auto moved = function;
    moved.source_file = "moved.qui";
    moved.source_line += 20;
    moved.source_column += 2;
    moved.declaration->source_file = "moved.qui";
    auto& location = std::get<SourceLocation>(moved.blocks[0].instructions[0]);
    location.position = {23,7};
    location.source_file = "moved.qui";
    location.source_revision = "new revision";
    location.node_id = "new node";
    std::get<Binary>(moved.blocks[0].instructions[2]).position = {23,11};
    const auto after = identify(moved);
    require(before.structural == after.structural, "source edits changed structural hash");
    require(before.locations != after.locations, "source edits did not change location hash");
    const auto encoded = canonical_encode(moved);
    const auto neutral = location_neutral(moved);
    require(neutral.blocks[0].instructions.size() == moved.blocks[0].instructions.size() &&
            std::holds_alternative<SourceLocation>(neutral.blocks[0].instructions[0]),
            "neutral IR deleted a source slot");
    require(identify(neutral).structural == after.structural, "neutral IR changed structural hash");
    const auto restored = stitch(neutral,encoded.locations);
    require(identify(restored) == after, "stitch lost source attribution");
    require(restored.source_file == moved.source_file &&
            restored.declaration->source_file == moved.declaration->source_file &&
            std::get<Binary>(restored.blocks[0].instructions[2]).position.line == 23 &&
            std::get<SourceLocation>(restored.blocks[0].instructions[0]).node_id == "new node",
            "stitch did not restore exact source fields");
    auto changed = moved;
    std::get<Binary>(changed.blocks[0].instructions[2]).op = "-";
    require(identify(changed).structural != after.structural, "semantic edit was omitted");
    changed = moved;
    changed.declaration->signature += ".";
    require(identify(changed).structural != after.structural, "declaration contract was omitted");
    changed = moved;
    changed.tensor_regions.push_back({});
    require(identify(changed).structural != after.structural, "region metadata was omitted");
    auto incomplete = encoded.locations;
    incomplete.positions.pop_back();
    bool rejected = false;
    try { (void)stitch(neutral,incomplete); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "stitch accepted an incomplete location table");

    SemanticWithSpan first{true,{{4,2,3},{8,2,7}}};
    auto second = first;
    second.site = {{40,20,30},{80,20,70}};
    require(canonical_encode_value(first).bytes == canonical_encode_value(second).bytes,
            "semantic helper SourceSpan entered structural encoding");
    second.writes = false;
    require(canonical_encode_value(first).bytes != canonical_encode_value(second).bytes,
            "semantic helper field was omitted with its span");
    require(canonical_encode_value(first).locations.spans[0].end.offset == 8,
            "semantic helper span offsets were lost");

    auto shaped = Type::tensor(Type::simple(TypeKind::Real32));
    shaped.tensor_known_shape_prefix = {ShapeExtent::symbol(37),
        extent_arithmetic("/",ShapeExtent::symbol(37),ShapeExtent(2))};
    auto renamed = shaped;
    renamed.tensor_known_shape_prefix = {ShapeExtent::symbol(91),
        extent_arithmetic("/",ShapeExtent::symbol(91),ShapeExtent(2))};
    require(canonical_encode_value(shaped).bytes == canonical_encode_value(renamed).bytes,
            "typed shape symbol alpha-renaming changed encoding");
    renamed.tensor_known_shape_prefix[1].atom_operations.begin()->second = "%";
    require(canonical_encode_value(shaped).bytes != canonical_encode_value(renamed).bytes,
            "actual shape atom metadata was omitted");

    InlineScope scope;
    scope.identity = 1;
    scope.callee = "helper";
    scope.source_file = "helper.qui";
    scope.position = {4,2};
    scope.blocks = {"entry"};
    scope.slots.push_back({"inline.value", integer, InlineSlotKind::OwnedLocal, "value"});
    moved.inline_scopes.push_back(scope);
    const auto inline_before = identify(moved);
    moved.inline_scopes[0].source_file = "relocated.qui";
    moved.inline_scopes[0].position = {40,20};
    moved.inline_scopes[0].slots[0].source_name = "renamed source variable";
    const auto inline_after = identify(moved);
    require(inline_before.structural == inline_after.structural &&
            inline_before.locations != inline_after.locations,
            "inline provenance entered structural encoding");
    const auto inline_encoding = canonical_encode(moved);
    require(identify(stitch(location_neutral(moved), inline_encoding.locations)) == inline_after,
            "stitch lost inline source provenance");
    moved.inline_scopes[0].slots[0].name += ".changed";
    require(identify(moved).structural != inline_after.structural,
            "inline storage identity was omitted");
}
} // namespace
int main() {
    try {
        mutation_and_stitch();
        std::cout << "IR identity: semantic changes, location slots and exact stitch verified\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
