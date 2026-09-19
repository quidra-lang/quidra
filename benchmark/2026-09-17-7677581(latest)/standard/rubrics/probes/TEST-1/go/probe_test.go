// TEST-1 probe, Go. Exactly 3 tests using the first-party testing package:
// two assert a true condition, one asserts a false condition.
package test1go

import "testing"

func TestPassOne(t *testing.T) {
	if !(Add(1, 1) == 2) {
		t.Errorf("assertion failed: Add(1,1) == 2")
	}
}

func TestPassTwo(t *testing.T) {
	if !(Add(2, 3) == 5) {
		t.Errorf("assertion failed: Add(2,3) == 5")
	}
}

func TestFailOne(t *testing.T) {
	if !(Add(1, 1) == 3) {
		t.Errorf("assertion failed: Add(1,1) == 3")
	}
}
