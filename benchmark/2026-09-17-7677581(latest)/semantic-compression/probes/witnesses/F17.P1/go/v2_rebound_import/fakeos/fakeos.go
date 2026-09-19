package fakeos

type Thing struct{ payload string }

func Open(name string) (*Thing, error) { return &Thing{payload: "abcdefghij"}, nil }

func (t *Thing) Close() error { return nil }
