use test2_rust::mod_a::scale;
use test2_rust::mod_b::scale_and_offset;

#[test]
fn scale_works() {
    assert_eq!(scale(2), 6);
}

#[test]
fn scale_and_offset_works() {
    assert_eq!(scale_and_offset(2), 7);
}
