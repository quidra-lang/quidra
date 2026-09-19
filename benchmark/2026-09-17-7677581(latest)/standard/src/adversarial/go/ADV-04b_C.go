package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	v := int32(-1) < uint32(1)
	fmt.Printf("OBS=CMP:%t\n", v)
	fmt.Println("ADV-END")
}
