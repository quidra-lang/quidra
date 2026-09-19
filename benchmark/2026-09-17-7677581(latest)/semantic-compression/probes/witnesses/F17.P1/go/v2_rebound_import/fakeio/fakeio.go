package fakeio

import "w17/fakeos"

func ReadAll(t *fakeos.Thing) ([]byte, error) { return []byte("abcdefghij"), nil }
