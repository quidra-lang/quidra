private func privFn() -> Int { 7 }        // non-public: private to this file/module
public  func pubFn()  -> Int { privFn() } // public: visible to importing modules
