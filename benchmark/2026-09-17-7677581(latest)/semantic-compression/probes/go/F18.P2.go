package main

import "fmt"

// BEGIN PROBE F18.P2
import "probe/util"

func run() int32 {
	result := util.PubAdd(2, 3)
	return result
	// END PROBE F18.P2
}

func main() {
	fmt.Println(run())
}
