package main

import (
	"errors"
	"fmt"
	"strconv"
)

func parseTwice(s string) (int32, error) {
	v, err := strconv.ParseInt(s, 10, 32)
	if err != nil {
		return 0, err
	}
	return int32(v) * 2, nil
}

func main() {
	for _, s := range []string{"21", "zz", "99999999999", "2000000000"} {
		n, err := parseTwice(s)
		fmt.Printf("%q -> n=%d err=%v range=%v syntax=%v\n", s, n, err,
			errors.Is(err, strconv.ErrRange), errors.Is(err, strconv.ErrSyntax))
	}
}
