package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	v := int32(float64(1e30))
	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
