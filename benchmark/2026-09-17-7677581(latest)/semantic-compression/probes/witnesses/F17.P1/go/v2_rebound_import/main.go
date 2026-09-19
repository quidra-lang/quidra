package main

import (
	"fmt"
	io "w17/fakeio"
	os "w17/fakeos"
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
	fmt.Println("no-file-opened result:", n, err)
}
