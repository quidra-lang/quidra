package x13
func privFn() int { return 7 }        // non-public: lowercase identifier, not exported
func PubFn() int { return privFn() }  // public: exported identifier
