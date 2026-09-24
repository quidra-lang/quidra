package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
)

func main() {
	fmt.Println("ADV-START")
	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	n, _ := strconv.ParseInt(sc.Text(), 10, 64)
	xs := make([]int64, n)
	xs[0] = 1
	first := xs[0]
	fmt.Printf("OBS=ALLOC:%d|FIRST:%d\n", len(xs), first)
	fmt.Println("ADV-END")
}
