package main

import "fmt"

func probe() int32 {
	// BEGIN PROBE F05.P2
	put := func(p *int32) { *p = 12 }
	var cell int32 = 3
	put(&cell)
	return cell
	// END PROBE F05.P2
}

func main() {
	fmt.Println(probe())
}
