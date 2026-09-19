package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	xs := []int64{10, 20, 30, 40, 50}
	e := xs[-1]
	fmt.Printf("OBS=ELEM:%d\n", e)
	fmt.Println("ADV-END")
}
