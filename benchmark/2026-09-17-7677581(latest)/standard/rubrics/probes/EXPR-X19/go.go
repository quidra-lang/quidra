package main
import ("fmt"; "sync")
func main() { var wg sync.WaitGroup; box := 0; wg.Add(1)
	go func() { defer wg.Done(); box = 42 }(); wg.Wait(); fmt.Println("X19", box) }
