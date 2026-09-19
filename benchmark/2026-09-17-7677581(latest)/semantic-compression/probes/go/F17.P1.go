package main

import (
	"fmt"
	"io"
	"os"
)

// BEGIN PROBE F17.P1
func readAll() (int, error) {
	f, err := os.Open("data.txt")
	if err != nil {
		return 0, err
	}
	defer f.Close()
	b, err := io.ReadAll(f)
	if err != nil {
		return 0, err
	}
	text := string(b)
	return len(text), nil
}

// END PROBE F17.P1

func main() {
	n, err := readAll()
	if err != nil {
		panic(err)
	}
	fmt.Println(n)
}
