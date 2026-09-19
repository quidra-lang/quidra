package main

/*
#cgo CFLAGS: -I../c
#include "supplied.h"
*/
import "C"
import "fmt"

func main() {
	r := C.SuppliedRecord{count: 3, weight: 1.5}
	fmt.Printf("supplied_add=%d\n", int(C.supplied_add(20, 22)))
	fmt.Printf("supplied_weighted=%.2f\n", float64(C.supplied_weighted(&r)))
	fmt.Printf("SUPPLIED_SCALE=%d\n", int(C.SUPPLIED_SCALE))
}
