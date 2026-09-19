package main

import "strconv"

type S string

func f(s S) { strconv.ParseInt(s, 10, 32) }

func main() { f("1") }
