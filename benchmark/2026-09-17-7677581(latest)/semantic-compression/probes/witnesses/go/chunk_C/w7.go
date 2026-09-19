package main

func bad[T any](a, b T) T {
	if a > b {
		return a
	}
	return b
}

func main() { _ = bad[int](1, 2) }
