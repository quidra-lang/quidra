#include "cpp_x13.h"
namespace { int priv(){ return 7; } }   // non-public: internal linkage (unnamed namespace)
namespace x13 { int pub(){ return priv(); } }
