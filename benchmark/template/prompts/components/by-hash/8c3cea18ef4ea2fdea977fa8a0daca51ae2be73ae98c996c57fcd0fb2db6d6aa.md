

---

## Embedded task input: /quidra-benchmark/work/audit/semantic-compression/canonical_fragments_rust.json
Source SHA-256: `25e3bc77fc9de059f171f12820c6ca714418c6977948df20f3ab7fdd16f09d8e`

{
  "canonical_fragments": {
    "F01.P1": {
      "citation": "Rust Reference 10.1 Variable declarations (let)",
      "fragment": "let n = 7;\nn",
      "justification": "Rust Reference 10.1 Variable declarations (let)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F01.P2": {
      "citation": "Rust Reference 10.2.1 Static items; 'static mut' items",
      "fragment": "static mut counter: i64 = 0;\nconst LIMIT: i64 = 100;",
      "justification": "Rust Reference 10.2.1 Static items; 'static mut' items",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F02.P1": {
      "citation": "Rust Reference 6.3 Variables; borrow/initializedness checker",
      "fragment": "let v: i32;\nif cond { v = 5; } else { v = 9; }\nv",
      "justification": "Rust Reference 6.3 Variables; borrow/initializedness checker",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F02.P2": {
      "citation": "std::mem::MaybeUninit documentation",
      "fragment": "let mut buf: [MaybeUninit<u8>; 16] = unsafe { MaybeUninit::uninit().assume_init() };\nbuf[0].write(1);\nunsafe { buf[0].assume_init() }",
      "justification": "std::mem::MaybeUninit documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F03.P1": {
      "citation": "Rust Reference 8.2.4 Arithmetic and logical operators / compound assignment",
      "fragment": "x += 1;",
      "justification": "Rust Reference 8.2.4 Arithmetic and logical operators / compound assignment",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F03.P2": {
      "citation": "std::ops::IndexMut for Vec<T>",
      "fragment": "xs[1] = 42;",
      "justification": "std::ops::IndexMut for Vec<T>",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F04.P1": {
      "citation": "Rust Reference 10.3 References; mutable borrows",
      "fragment": "let mut slot: i32 = 4;\nlet port = &mut slot;\n*port = 9;\nslot",
      "justification": "Rust Reference 10.3 References; mutable borrows",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F04.P2": {
      "citation": "Rust Reference slices (&[T])",
      "fragment": "let window: &[i32] = &xs;\nwindow[0]",
      "justification": "Rust Reference slices (&[T])",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F04.P3": {
      "citation": "std::rc::Rc::ptr_eq documentation",
      "fragment": "let first = Rc::new(RefCell::new(5));\nlet mut v = Vec::new();\nv.push(first.clone());\nlet second = v.pop().unwrap();\nlet same = Rc::ptr_eq(&first, &second);\nsame",
      "justification": "std::rc::Rc::ptr_eq documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F05.P1": {
      "citation": "Rust Reference 6.5 Call expressions; ownership/move semantics",
      "fragment": "let x = vec![1, 2, 3];\nf(x);\nx[0]",
      "justification": "Rust Reference 6.5 Call expressions; ownership/move semantics",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F05.P2": {
      "citation": "Rust Reference mutable references",
      "fragment": "fn put(x: &mut i32) { *x = 12; }\nlet mut cell: i32 = 3;\nput(&mut cell);\ncell",
      "justification": "Rust Reference mutable references",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F05.P3": {
      "citation": "Rust Reference 6.9 Closures; Iterator::map",
      "fragment": "let neg = |a: i32| k - a;\nlet ys: Vec<i32> = xs.into_iter().map(neg).collect();\nys[0]",
      "justification": "Rust Reference 6.9 Closures; Iterator::map",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F06.P1": {
      "citation": "Rust Reference 6.6 Return expressions; Copy semantics for i32",
      "fragment": "fn mid(xs: Vec<i32>) -> i32 { xs[1] }\nlet xs = vec![1, 2, 3];\nlet y = mid(xs);\ny",
      "justification": "Rust Reference 6.6 Return expressions; Copy semantics for i32",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F06.P2": {
      "citation": "Rust Reference tuple types",
      "fragment": "fn divmod2(a: i32, b: i32) -> (i32, i32) { (a / b, a % b) }\nlet (q, r) = divmod2(a, b);\nq + r",
      "justification": "Rust Reference tuple types",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F07.P1": {
      "citation": "Rust Reference operator precedence",
      "fragment": "let r = a * b + c;",
      "justification": "Rust Reference operator precedence",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F07.P2": {
      "citation": "Rust Reference integer division/remainder semantics (truncation toward zero)",
      "fragment": "let q = a / b;\nlet m = a % b;\nlet d = a as f64 / b as f64;",
      "justification": "Rust Reference integer division/remainder semantics (truncation toward zero)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F08.P1": {
      "citation": "i32::checked_add documentation",
      "fragment": "let m: i32 = i32::MAX;\nlet o = m.checked_add(1).unwrap_or(0);\no",
      "justification": "i32::checked_add documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F08.P2": {
      "citation": "i32::checked_div documentation",
      "fragment": "let q = a.checked_div(b).unwrap_or(0);\nq",
      "justification": "i32::checked_div documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F09.P1": {
      "citation": "impl PartialEq for str/String (content comparison)",
      "fragment": "let eq = s1 == s2;",
      "justification": "impl PartialEq for str/String (content comparison)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F09.P2": {
      "citation": "slice::sort_by documentation (in-place)",
      "fragment": "xs.sort_by(|a, b| b.cmp(a));\nlet lt = a < b;",
      "justification": "slice::sort_by documentation (in-place)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F10.P1": {
      "citation": "TryFrom<i64> for i32 documentation",
      "fragment": "let small: i32 = big.try_into().unwrap_or(0);\nsmall",
      "justification": "TryFrom<i64> for i32 documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F10.P2": {
      "citation": "Rust Reference: no implicit numeric coercions; explicit 'as' conversion required",
      "fragment": "let sum = i as f64 + d;",
      "justification": "Rust Reference: no implicit numeric coercions; explicit 'as' conversion required",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F11.P1": {
      "citation": "Rust Reference: indexing panics on out-of-bounds",
      "fragment": "let e = xs[i];\ne",
      "justification": "Rust Reference: indexing panics on out-of-bounds",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F11.P2": {
      "citation": "slice indexing / range syntax (shares storage)",
      "fragment": "let part = &xs[1..4];\npart[0]",
      "justification": "slice indexing / range syntax (shares storage)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F12.P1": {
      "citation": "std::option::Option documentation",
      "fragment": "let o: Option<i32> = None;\nlet n: i32 = o.unwrap_or(0);\nn",
      "justification": "std::option::Option documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F12.P2": {
      "citation": "Option::map documentation",
      "fragment": "let h = |a: i32| a + 1;\nlet p: Option<i32> = o.map(h);\np.unwrap_or(0)",
      "justification": "Option::map documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F13.P1": {
      "citation": "'?' operator (Rust Reference 6.7); str::parse",
      "fragment": "fn parse_twice(s: &str) -> Result<i32, std::num::ParseIntError> { let v: i32 = s.parse()?; Ok(v * 2) }",
      "justification": "'?' operator (Rust Reference 6.7); str::parse",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F13.P2": {
      "citation": "Result::unwrap_or documentation",
      "fragment": "let n: i32 = s.parse().unwrap_or(0);\nn",
      "justification": "Result::unwrap_or documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F14.P1": {
      "citation": "Rust Reference 6.1.1 Enumerations (closed set)",
      "fragment": "enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }\nlet s: Shape = Shape::Circle { r: 2.0 };",
      "justification": "Rust Reference 6.1.1 Enumerations (closed set)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F14.P2": {
      "citation": "Rust Reference match exhaustiveness checking",
      "fragment": "let area: f64 = match s { Shape::Circle { r } => 3.141592653589793 * r * r, Shape::Rect { w, h } => w * h, };\narea",
      "justification": "Rust Reference match exhaustiveness checking",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F14.P3": {
      "citation": "Rust Reference trait objects (dyn Trait); multi-unit recipe frozen for this probe",
      "fragment": "[shapes.rs] pub trait Shape { fn area(&self) -> f64; } pub struct Circle { pub r: f64 } impl Shape for Circle {...} pub struct Rect {...} impl Shape for Rect {...} [main.rs] struct Tri { b: f64, hh: f64 } impl shapes::Shape for Tri {...} let t: Box<dyn shapes::Shape> = Box::new(Tri { b: 4.0, hh: 3.0 }); t.area()",
      "justification": "Rust Reference trait objects (dyn Trait); multi-unit recipe frozen for this probe",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F15.P1": {
      "citation": "Rust Reference generics/monomorphization",
      "fragment": "fn head<T: Copy>(xs: &[T]) -> T { xs[0] }\nlet a = head(&[4, 5, 6]);\nlet b = head(&[\"p\", \"q\"]);\nformat!(\"{}{}\", a, b)",
      "justification": "Rust Reference generics/monomorphization",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F15.P2": {
      "citation": "Rust Reference trait bounds (declaration-site constraint)",
      "fragment": "fn max_of<T: Ord>(a: T, b: T) -> T { if a > b { a } else { b } }\nlet m = max_of(3, 5);\nm",
      "justification": "Rust Reference trait bounds (declaration-site constraint)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F15.P3": {
      "citation": "Rust Reference trait objects, dynamic dispatch",
      "fragment": "trait Named { fn tag(&self) -> String; }\nstruct A; impl Named for A { fn tag(&self) -> String { \"a\".to_string() } }\nstruct B; impl Named for B { fn tag(&self) -> String { \"b\".to_string() } }\nlet items: Vec<Box<dyn Named>> = vec![Box::new(A), Box::new(B)];\nitems[0].tag()",
      "justification": "Rust Reference trait objects, dynamic dispatch",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F16.P1": {
      "citation": "Iterator::sum documentation",
      "fragment": "let xs = vec![1, 2, 3];\nlet total: i32 = xs.iter().sum();\ntotal",
      "justification": "Iterator::sum documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F16.P2": {
      "citation": "std::collections::HashMap documentation",
      "fragment": "let mut mp: HashMap<String, i32> = HashMap::new();\nmp.insert(\"a\".to_string(), 1);\nlet total: i32 = mp.values().sum();\nlet miss: i32 = *mp.get(\"b\").unwrap_or(&0);\ntotal + miss",
      "justification": "std::collections::HashMap documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F17.P1": {
      "citation": "std::fs::File Drop impl (RAII); '?' operator",
      "fragment": "fn read_all() -> std::io::Result<usize> { let mut text = String::new(); let mut f = std::fs::File::open(\"data.txt\")?; std::io::Read::read_to_string(&mut f, &mut text)?; Ok(text.len()) }",
      "justification": "std::fs::File Drop impl (RAII); '?' operator",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F17.P2": {
      "citation": "Drop trait documentation (deterministic, exactly-once, end-of-scope)",
      "fragment": "struct Handle { id: i32 }\nimpl Drop for Handle { fn drop(&mut self) { unsafe { RELEASED += 1; } } }\n{ let _h = Handle { id: 1 }; }\nunsafe { RELEASED }",
      "justification": "Drop trait documentation (deterministic, exactly-once, end-of-scope)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F18.P1": {
      "citation": "Rust std prelude documentation",
      "fragment": "println!(\"x\");",
      "justification": "Rust std prelude documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F18.P2": {
      "citation": "Rust Reference visibility and privacy (mod, pub)",
      "fragment": "[util.rs] pub fn pub_add(a: i32, b: i32) -> i32 { a + b + secret() } fn secret() -> i32 { 1 } [main.rs] mod util; util::pub_add(2, 3)",
      "justification": "Rust Reference visibility and privacy (mod, pub)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F19.P1": {
      "citation": "std::thread::spawn/JoinHandle documentation",
      "fragment": "let t1 = std::thread::spawn(|| 20);\nlet t2 = std::thread::spawn(|| 22);\nlet sum = t1.join().unwrap() + t2.join().unwrap();\nsum",
      "justification": "std::thread::spawn/JoinHandle documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F19.P2": {
      "citation": "std::sync::atomic::AtomicI64 documentation",
      "fragment": "let counter = std::sync::Arc::new(std::sync::atomic::AtomicI64::new(0)); let c1 = counter.clone(); let c2 = counter.clone(); let h1 = std::thread::spawn(move || { for _ in 0..1000 { c1.fetch_add(1, std::sync::atomic::Ordering::SeqCst); } }); let h2 = std::thread::spawn(move || { for _ in 0..1000 { c2.fetch_add(1, std::sync::atomic::Ordering::SeqCst); } }); h1.join().unwrap(); h2.join().unwrap(); counter.load(std::sync::atomic::Ordering::SeqCst)",
      "justification": "std::sync::atomic::AtomicI64 documentation",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F20.P1": {
      "citation": "Rust Reference 6.2 External blocks (extern)",
      "fragment": "extern \"C\" { fn abs(x: i32) -> i32; }\nlet r: i32 = unsafe { abs(-3) };\nr",
      "justification": "Rust Reference 6.2 External blocks (extern)",
      "level": "FULL",
      "none_reason": null,
      "partial_reasons": []
    },
    "F20.P2": {
      "citation": "Rust Reference #[no_mangle] attribute; rustc --crate-type documentation",
      "fragment": "#[no_mangle]\npub extern \"C\" fn add2(a: i32, b: i32) -> i32 { a + b }",
      "justification": "Rust Reference #[no_mangle] attribute; rustc --crate-type documentation",
      "level": "PARTIAL",
      "none_reason": null,
      "partial_reasons": [
        "P-b"
      ]
    }
  },
  "language": "Rust",
  "rule": "Use exactly these independently certified probe fragments for every downstream Semantic Compression metric. FULL/PARTIAL entries must be measured verbatim; NONE entries have no fragment and must not receive a numeric per-probe A/B/C/D measurement.",
  "schema_version": 2,
  "source_requirement_prefix": "annotation.canonical_fragment--",
  "source_work_unit_ids": [
    "sc-canonical-fragment--rust--f01-p1",
    "sc-canonical-fragment--rust--f01-p2",
    "sc-canonical-fragment--rust--f02-p1",
    "sc-canonical-fragment--rust--f02-p2",
    "sc-canonical-fragment--rust--f03-p1",
    "sc-canonical-fragment--rust--f03-p2",
    "sc-canonical-fragment--rust--f04-p1",
    "sc-canonical-fragment--rust--f04-p2",
    "sc-canonical-fragment--rust--f04-p3",
    "sc-canonical-fragment--rust--f05-p1",
    "sc-canonical-fragment--rust--f05-p2",
    "sc-canonical-fragment--rust--f05-p3",
    "sc-canonical-fragment--rust--f06-p1",
    "sc-canonical-fragment--rust--f06-p2",
    "sc-canonical-fragment--rust--f07-p1",
    "sc-canonical-fragment--rust--f07-p2",
    "sc-canonical-fragment--rust--f08-p1",
    "sc-canonical-fragment--rust--f08-p2",
    "sc-canonical-fragment--rust--f09-p1",
    "sc-canonical-fragment--rust--f09-p2",
    "sc-canonical-fragment--rust--f10-p1",
    "sc-canonical-fragment--rust--f10-p2",
    "sc-canonical-fragment--rust--f11-p1",
    "sc-canonical-fragment--rust--f11-p2",
    "sc-canonical-fragment--rust--f12-p1",
    "sc-canonical-fragment--rust--f12-p2",
    "sc-canonical-fragment--rust--f13-p1",
    "sc-canonical-fragment--rust--f13-p2",
    "sc-canonical-fragment--rust--f14-p1",
    "sc-canonical-fragment--rust--f14-p2",
    "sc-canonical-fragment--rust--f14-p3",
    "sc-canonical-fragment--rust--f15-p1",
    "sc-canonical-fragment--rust--f15-p2",
    "sc-canonical-fragment--rust--f15-p3",
    "sc-canonical-fragment--rust--f16-p1",
    "sc-canonical-fragment--rust--f16-p2",
    "sc-canonical-fragment--rust--f17-p1",
    "sc-canonical-fragment--rust--f17-p2",
    "sc-canonical-fragment--rust--f18-p1",
    "sc-canonical-fragment--rust--f18-p2",
    "sc-canonical-fragment--rust--f19-p1",
    "sc-canonical-fragment--rust--f19-p2",
    "sc-canonical-fragment--rust--f20-p1",
    "sc-canonical-fragment--rust--f20-p2"
  ]
}
