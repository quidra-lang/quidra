fn privFn() i32 { return 7; }            // non-public: not marked pub, invisible to importers
pub fn pubFn() i32 { return privFn(); }  // public
