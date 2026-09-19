package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Println("ADV-START")

	b, _ := os.ReadFile("inputs/ADV-23.bin")
	s := string(b)

	fmt.Printf("OBS=CP:%d\n", len(s))
	fmt.Println("ADV-END")
}
