package main
import ("fmt"; "regexp")
func main() { re := regexp.MustCompile(`(\d{4})-(\d{2})`); m := re.FindStringSubmatch("date 2026-09-17")
	fmt.Println("X21", m[1], m[2]) }
