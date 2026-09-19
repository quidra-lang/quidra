package main
import ("errors"; "fmt")
var ErrBoom = errors.New("boom")
func inner() error { return ErrBoom }
func outer() error { if err := inner(); err != nil { return fmt.Errorf("outer: %w", err) }; return nil }
func main() { err := outer(); if errors.Is(err, ErrBoom) { fmt.Println("X08 caught", ErrBoom) } }
