use std::os::raw::{c_int, c_void};

#[repr(C)]
#[derive(Copy, Clone)]
struct Record { count: i32, weight: f64 }

extern "C" {
    fn qsort(base: *mut c_void, n: usize, size: usize,
             cmp: extern "C" fn(*const c_void, *const c_void) -> c_int);
}

extern "C" fn cmp_double(a: *const c_void, b: *const c_void) -> c_int {
    let x = unsafe { *(a as *const f64) };
    let y = unsafe { *(b as *const f64) };
    if x > y { 1 } else if x < y { -1 } else { 0 }
}
extern "C" fn cmp_record(a: *const c_void, b: *const c_void) -> c_int {
    let x = unsafe { (*(a as *const Record)).count };
    let y = unsafe { (*(b as *const Record)).count };
    if x > y { 1 } else if x < y { -1 } else { 0 }
}

fn main() {
    println!("sizeof={} offset={}", std::mem::size_of::<Record>(),
             std::mem::offset_of!(Record, weight));
    let mut xs: [f64; 5] = [3.5, 1.25, 4.75, 1.5, 2.25];
    unsafe {
        qsort(xs.as_mut_ptr() as *mut c_void, 5, std::mem::size_of::<f64>(), cmp_double);
    }
    let s: Vec<String> = xs.iter().map(|v| format!("{:.2}", v)).collect();
    println!("{}", s.join(" "));
    let mut rs = [Record{count:3,weight:1.5}, Record{count:1,weight:4.0}, Record{count:2,weight:2.5}];
    unsafe {
        qsort(rs.as_mut_ptr() as *mut c_void, 3, std::mem::size_of::<Record>(), cmp_record);
    }
    let t: Vec<String> = rs.iter().map(|r| format!("{}:{:.2}", r.count, r.weight)).collect();
    println!("{}", t.join(" "));
}
