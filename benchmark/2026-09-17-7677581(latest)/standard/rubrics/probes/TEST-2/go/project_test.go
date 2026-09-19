package test2go

import (
	"testing"

	"test2go/moda"
	"test2go/modb"
)

func TestScale(t *testing.T) {
	if moda.Scale(2) != 6 {
		t.Errorf("Scale(2) != 6")
	}
}

func TestScaleAndOffset(t *testing.T) {
	if modb.ScaleAndOffset(2) != 7 {
		t.Errorf("ScaleAndOffset(2) != 7")
	}
}
