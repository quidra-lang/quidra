package main
import ("fmt"; "os")
func main() { os.WriteFile("x18.txt", []byte("hello"), 0644)
	d, _ := os.ReadFile("x18.txt")
	_, err := os.Stat("x18.txt")
	fmt.Println("X18", string(d), err == nil) }
