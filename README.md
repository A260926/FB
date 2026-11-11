# go-fuzz-headers
This repository contains various helper functions for go fuzzing. It is mostly used in combination with [go-fuzz](https://github.com/dvyukov/go-fuzz), but compatibility with fuzzing in the standard library will also be supported. Any coverage guided fuzzing engine that provides an array or slice of bytes can be used with go-fuzz-headers.


## Usage
Using go-fuzz-headers is easy. First create a new consumer with the bytes provided by the fuzzing engine:

```go
import (
	fuzz "github.com/AdaLogics/go-fuzz-headers"
)
data := []byte{'R', 'a', 'n', 'd', 'o', 'm'}
f := fuzz.NewConsumer(data)

```

This creates a `Consumer` that consumes the bytes of the input as it uses them to fuzz different types.

After that, `f` can be used to easily create fuzzed instances of different types. Below are some examples:

### Structs
One of the most useful features of go-fuzz-headers is its ability to fill structs with the data provided by the fuzzing engine. This is done with a single line:
```go
type Person struct {
    Name string
    Age  int
}
p := Person{}
// Fill p with values based on the data provided by the fuzzing engine:
err := f.GenerateStruct(&p)
```

This includes nested structs too. In this example, the fuzz Consumer will also insert values in `p.BestFriend`:
```go
type PersonI struct {
    Name       string
    Age        int
    BestFriend PersonII
}
type PersonII struct {
    Name string
    Age  int
}
p := PersonI{}
err := f.GenerateStruct(&p)
```

If the consumer should insert values for unexported fields as well as exported, this can be enabled with:

```go
f.AllowUnexportedFields()
```

...and disabled with:

```go
f.DisallowUnexportedFields()
```

### Custom generators

`GenerateWithCustom` works like `GenerateStruct`, but lets registered
functions take complete control of values whose exact type matches. A
generator takes a pointer (or a map) and a `Continue` handle:

```go
type Header struct {
    Kind    byte
    Payload byte
}

type Packet struct {
    Hdr  Header
    Tail byte
}

f.AddFuncs([]interface{}{func(h *Header, c fuzz.Continue) error {
    // Consume bytes from the same consumer; the handler fully owns
    // this value, so default generation will not touch it afterwards.
    kind, err := c.F.GetByte()
    if err != nil {
        return err
    }
    payload, err := c.F.GetByte()
    if err != nil {
        return err
    }
    h.Kind, h.Payload = kind, payload
    return nil
}})

var p Packet
// Hdr is filled by the handler; Tail keeps using default generation
// and reads exactly the next byte after the handler's bytes.
err := f.GenerateWithCustom(&p)
```

Matching is by exact type: a type alias is the same type as its
target, while a separately defined named type only matches a handler
registered for that named type. Values without a handler are generated
as usual. Inside a handler, `c.GenerateStruct` fills a subtree with
default generation while `c.GenerateStructWithCustom` keeps the
registered handlers active; both share the handler's consumer, so
sibling fields continue at the next byte. A handler error is wrapped
in `ErrCustomFunction` and remains recognizable with `errors.Is`.

### Other types:

Other useful APIs:

```go
createdString, err := f.GetString() // Gets a string
createdInt, err := f.GetInt() // Gets an integer
createdByte, err := f.GetByte() // Gets a byte
createdBytes, err := f.GetBytes() // Gets a byte slice
createdBool, err := f.GetBool() // Gets a boolean
err := f.FuzzMap(target_map) // Fills a map
createdTarBytes, err := f.TarBytes() // Gets bytes of a valid tar archive
err := f.CreateFiles(inThisDir) // Fills inThisDir with files
createdString, err := f.GetStringFrom("anyCharInThisString", ofThisLength) // Gets a string that consists of chars from "anyCharInThisString" and has the exact length "ofThisLength"
```

Most APIs are added as they are needed.

## Projects that use go-fuzz-headers
- [runC](https://github.com/opencontainers/runc)
- [Istio](https://github.com/istio/istio)
- [Vitess](https://github.com/vitessio/vitess)
- [Containerd](https://github.com/containerd/containerd)

Feel free to add your own project to the list, if you use go-fuzz-headers to fuzz it.


 

## Status
The project is under development and will be updated regularly.

## References
go-fuzz-headers' approach to fuzzing structs is strongly inspired by [gofuzz](https://github.com/google/gofuzz).