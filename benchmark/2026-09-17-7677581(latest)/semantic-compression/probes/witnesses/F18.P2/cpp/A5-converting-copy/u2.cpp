#include "u2.h"
namespace util { namespace { double secret() { return 0.5; } }
double pub_add(double a, double b) { return a + b + secret(); } }
