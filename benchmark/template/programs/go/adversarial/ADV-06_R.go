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
	m := a / b
	xs := []float64{3.0, m, 1.0}
	best := xs[0]
	for _, x := range xs[1:] {
		if x > best {
			best = x
		}
	}
	selfeq := m == m
	fmt.Printf("OBS=MAX:%.6f|SELFEQ:%t\n", best, selfeq)
	fmt.Println("ADV-END")
}
