package other

import "w/sh"

type Tri struct{ B float64 }

func (Tri) isShape() {}

var _ sh.Shape = Tri{1}
