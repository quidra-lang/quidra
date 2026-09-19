package main

import (
	"bufio"
	"fmt"
	"os"
)

func pick(b bool) int64 {
	if b {
		return 1
	}
}

func main() {
	fmt.Println("ADV-START")

	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	b := sc.Text() == "1"

	r := pick(b) * 2

	fmt.Printf("OBS=R:%d\n", r)
	fmt.Println("ADV-END")
}
