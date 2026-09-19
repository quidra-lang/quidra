package main
import ("fmt"; "unicode/utf8")
func main() { t := "h\u00e9llo"; n := utf8.RuneCountInString(t); fmt.Println("X15", n) }
