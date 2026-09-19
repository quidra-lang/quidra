#[derive(Clone, Copy)]
struct Cell(u8);
impl Cell {
    fn checked_div(self, o: Cell) -> Option<u8> { println!("user checked_div"); Some(self.0 + o.0) }
}

fn guarded(a: Cell, b: Cell) -> u8 {
// BEGIN PROBE F08.P2
let q = a.checked_div(b).unwrap_or(0);
q
// END PROBE F08.P2
}

fn main() {
    println!("{}", guarded(Cell(7), Cell(0)));
}
