package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	q := int64(7) / int64(0)
	fmt.Printf("OBS=QUOT:%d\n", q)
	fmt.Println("ADV-END")
}
