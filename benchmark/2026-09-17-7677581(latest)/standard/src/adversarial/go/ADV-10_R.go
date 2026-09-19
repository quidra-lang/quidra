package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
)

func main() {
	fmt.Println("ADV-START")
	xs := []int64{10, 20, 30, 40, 50}
	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	p, _ := strconv.ParseInt(sc.Text(), 10, 64)
	i := int(p)
	e := xs[i]
	fmt.Printf("OBS=ELEM:%d\n", e)
	fmt.Println("ADV-END")
}
