package main

import "fmt"

type P struct{ x int }

func v1(xs []int32, i int) int32       { e := xs[i]; return e }
func v2(xs []uint64, i int) uint64     { e := xs[i]; return e }
func v3(xs []float64, i int) float64   { e := xs[i]; return e }
func v4(xs []string, i int) string     { e := xs[i]; return e }
func v5(xs []any, i int) any           { e := xs[i]; return e }
func v6(xs []P, i int) P               { e := xs[i]; return e }
func v7(xs []*int, i int) *int         { e := xs[i]; return e }
func v8(xs map[int]int32, i int) int32 { e := xs[i]; return e }
func vs(xs string, i int) byte         { e := xs[i]; return e }

func main() {
	n := 7
	fmt.Println("V1", v1([]int32{1, 2, 3}, 2))
	fmt.Println("V2", v2([]uint64{18446744073709551615, 2, 3}, 0))
	fmt.Println("V3", v3([]float64{1.5, 2.5, 3.5}, 2))
	fmt.Println("V4", v4([]string{"p", "q", "r"}, 2))
	fmt.Println("V5", v5([]any{1, 2, 3}, 2))
	fmt.Println("V6", v6([]P{{1}, {2}, {3}}, 2))
	fmt.Println("V7", v7([]*int{nil, nil, &n}, 2) != nil)
	fmt.Println("V8-miss", v8(map[int]int32{0: 1}, 999))
	fmt.Println("VS", vs("abc", 2))
	defer func() { fmt.Println("V1-oob recovered:", recover()) }()
	fmt.Println(v1([]int32{1, 2, 3}, 999))
}
