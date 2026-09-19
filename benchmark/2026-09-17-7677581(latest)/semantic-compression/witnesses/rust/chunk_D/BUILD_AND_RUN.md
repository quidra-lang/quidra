# Witnesses and refutation witnesses - Rust, chunk D (families 16-20)

Build command for every file below is the frozen Semantic Compression recipe for Rust
(`environment/environment.json -> semantic_compression_recipes.recipes.rust`):

    rustc -O -C debug-assertions=on FILE.rs -o BIN        # run: ./BIN

(The file is copied to a name without a `.` before building, because rustc rejects `.` in an
inferred crate name; the bytes are unchanged. The measured fragment inside each file is
byte-identical to the corresponding frozen probe in ../../../probes/rust/.)

| File | Probe | Class | Build | Observed output |
|---|---|---|---|---|
| f16p1_V2_macro_shadow.rs | F16.P1 | V2 (counted branch) | ok | `shadowed 6` - a `macro_rules! vec` above the fragment shadows the prelude macro, and the same tokens now write to stdout |
| f16p2_V2_rebound_import.rs | F16.P2 | V2 (counted branch) | ok | `user-map 1` - `HashMap` rebound by an outside `use` to a user type; no hash table exists |
| f17p1_V2_rebound_import.rs | F17.P1 | V2 (counted branch) | ok | `no-fd 3` - `File`/`io` rebound by outside `use` items; no descriptor is acquired or released |
| f17p2_V2_shadowed_drop.rs | F17.P2 | V2 (counted branch) | ok (2 dead-code warnings) | `0` - a user trait named `Drop` is in scope, so the impl attaches no destructor and no cleanup runs |
| f17p2_V3_rebound_counter.rs | F17.P2 | V3 (counted branch) | ok | `hooked 7` - the given-context counter, `AtomicI32` and `Ordering` all rebound outside the fragment |
| f18p1_V2_macro_shadow.rs | F18.P1 | V2 (counted branch) | ok | stdout empty, stderr `stderr-instead` - a `macro_rules! println` above the fragment redirects the write |
| f18p2_refute_private.rs | F18.P2 | refutation (excluded class) | FAILS | `error[E0603]: function `secret` is private` - the visibility boundary is enforced by the language |
| f19p1_refute_std_shadow.rs | F19.P1, F19.P2 | refutation (excluded class) | FAILS | `error[E0260]: the name `std` is defined multiple times` - a leading `std` path segment cannot be rebound |
| f20p1_V2_symbol_interposed.rs | F20.P1 | V2 (counted branch) | ok | `hooked 99` - a `#[no_mangle] extern "C" fn abs` elsewhere in the program takes over the foreign call |
| f20p2_V1_overflow_abort.rs | F20.P2 | V1 (the admissible mode's edge behaviour) | ok | `attempt to add with overflow` then `panic in a function that cannot unwind ... aborting`, exit 134. For contrast only, NOT an admissible recipe entry: the same source built with `rustc -O` alone prints `-2147483648`. |

`data.txt` (6 bytes, `hello\n`) is the same content as the frozen Python and Quidra probe directories.
