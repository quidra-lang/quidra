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
	a, _ := strconv.ParseFloat(sc.Text(), 64)
	sc.Scan()
	b, _ := strconv.ParseFloat(sc.Text(), 64)
	h := a / b
	mean := (1.0 + h + 3.0) / 3.0
	diff := h - h
	fmt.Printf("OBS=MEAN:%.6f|DIFF:%.6f\n", mean, diff)
	fmt.Println("ADV-END")
}
