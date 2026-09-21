package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	v := uint32(0) - uint32(1)
	fmt.Printf("OBS=SUB:%d\n", v)
	fmt.Println("ADV-END")
}
