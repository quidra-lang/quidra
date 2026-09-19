// BEGIN PROBE F14.P3
pub const Shape = struct {
    ptr: *const anyopaque,
    areaFn: *const fn (*const anyopaque) f64,
    pub fn area(self: Shape) f64 {
        return self.areaFn(self.ptr);
    }
};
pub const Circle = struct {
    r: f64,
    pub fn area(p: *const anyopaque) f64 {
        const self: *const Circle = @ptrCast(@alignCast(p));
        return 3.141592653589793 * self.r * self.r;
    }
};
pub const Rect = struct {
    w: f64,
    h: f64,
    pub fn area(p: *const anyopaque) f64 {
        const self: *const Rect = @ptrCast(@alignCast(p));
        return self.w * self.h;
    }
};
// END PROBE F14.P3
