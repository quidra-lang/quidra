package x13
private fun privFn(): Int = 7          // non-public: file-private
public fun pubFn(): Int = privFn()     // public
