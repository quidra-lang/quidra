package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	v := int32(int64(9223372036854775807))
	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
