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
	f, _ := strconv.ParseFloat(sc.Text(), 64)
	v := int32(f)
	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
