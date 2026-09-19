package main

/*
#include <stdlib.h>
typedef int (*cmp_t)(const void*, const void*);
extern int ffi3_cmp_double(void*, void*);
extern int ffi3_cmp_record(void*, void*);
*/
import "C"

import (
	"fmt"
	"strings"
	"unsafe"
)

type Record struct {
	count  int32
	weight float64
}

//export ffi3_cmp_double
func ffi3_cmp_double(a unsafe.Pointer, b unsafe.Pointer) C.int {
	x := *(*float64)(a)
	y := *(*float64)(b)
	if x > y {
		return 1
	} else if x < y {
		return -1
	}
	return 0
}

//export ffi3_cmp_record
func ffi3_cmp_record(a unsafe.Pointer, b unsafe.Pointer) C.int {
	x := (*Record)(a).count
	y := (*Record)(b).count
	if x > y {
		return 1
	} else if x < y {
		return -1
	}
	return 0
}

func main() {
	var r Record
	fmt.Printf("sizeof=%d offset=%d\n", unsafe.Sizeof(r), unsafe.Offsetof(r.weight))

	xs := [5]float64{3.5, 1.25, 4.75, 1.5, 2.25}
	C.qsort(unsafe.Pointer(&xs[0]), C.size_t(len(xs)), C.size_t(unsafe.Sizeof(xs[0])),
		C.cmp_t(unsafe.Pointer(C.ffi3_cmp_double)))
	parts := make([]string, 0, 5)
	for _, v := range xs {
		parts = append(parts, fmt.Sprintf("%.2f", v))
	}
	fmt.Println(strings.Join(parts, " "))

	rs := [3]Record{{3, 1.5}, {1, 4.0}, {2, 2.5}}
	C.qsort(unsafe.Pointer(&rs[0]), C.size_t(len(rs)), C.size_t(unsafe.Sizeof(rs[0])),
		C.cmp_t(unsafe.Pointer(C.ffi3_cmp_record)))
	parts2 := make([]string, 0, 3)
	for _, v := range rs {
		parts2 = append(parts2, fmt.Sprintf("%d:%.2f", v.count, v.weight))
	}
	fmt.Println(strings.Join(parts2, " "))
}
