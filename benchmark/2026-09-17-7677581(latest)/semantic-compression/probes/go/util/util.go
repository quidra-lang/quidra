package util

// BEGIN PROBE F18.P2
func PubAdd(a, b int32) int32 { return a + b + secret() }

func secret() int32 { return 1 }

// END PROBE F18.P2
