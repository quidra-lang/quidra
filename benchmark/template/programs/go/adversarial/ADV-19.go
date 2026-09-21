package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
)

func f(n int64) int64 {
	return 1 + f(n+1)
}

func main() {
	fmt.Println("ADV-START")

	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	n, _ := strconv.ParseInt(sc.Text(), 10, 64)

	r := f(n)

	fmt.Printf("OBS=R:%d\n", r)
	fmt.Println("ADV-END")
}
