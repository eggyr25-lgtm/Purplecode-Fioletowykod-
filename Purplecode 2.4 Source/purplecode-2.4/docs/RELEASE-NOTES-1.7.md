# Purplecode Milestone 1 1.7 — Structs and Larger Programs

This release is **Milestone 1 1.7**. It builds directly on 1.6 and focuses on making the language practical for bigger, data-oriented programs.

## Headline feature: Structs

User-defined aggregate types are now supported on the hosted path:

```purple
struct Point {
    x: int;
    y: int;
}

struct Entity {
    id: int;
    pos: Point;
    health: int;
    name: string;
}

fn main() {
    let p: Point;
    p.x = 10;
    p.y = 20;

    let e: Entity;
    e.id = 1;
    e.pos.x = 5;
    e.pos.y = 12;
    e.health = 100;
    e.name = "Hero";

    Data.ShellPrint.Int(e.pos.x);
    return 0;
}
```

### Struct rules (1.7)

- Struct definitions appear at top level (before or among functions).
- Fields may be `int`, `bool`, `string`, or other previously defined structs (nesting is supported).
- Layout is straightforward: every field is 8-byte aligned. Size of a struct is the sum of its field sizes (each 8 bytes in the current model).
- Local variables of struct type are allocated on the stack.
- Field access uses the familiar `obj.field` syntax. Nested access (`obj.inner.field`) works.
- Assignment to a field is written `obj.field = expr;`.
- Struct values are currently best used via locals and field access. Full pointer-to-struct and heap allocation are planned for later milestones.

## Other improvements for larger programs

- Clearer type and field-access diagnostics.
- Predictable frame layout when structs are used.
- Documentation updated: `docs/LANGUAGE.md` now covers the struct system.
- New example: `examples/structs_demo.purple`.

## Compatibility

- All 1.6 language features and APIs continue to work unchanged.
- Existing `.purple` programs that do not use the new `struct` keyword require no changes.
- Boot / UEFI path (`purplebc`) does not yet expose the full struct feature set; structs are a hosted-compiler feature in 1.7.

## Struct feature status (honest)

| Feature                        | Status   | Notes                                              |
|--------------------------------|----------|----------------------------------------------------|
| Full field-offset lowering     | Done     | reads + writes + nested                            |
| Stack structs + zeroing        | Done     |                                                    |
| Heap structs (`Struct.New`)    | Done     | `Struct.New(Type)` / `Struct.Free`; pointer field access |
| Arrays of structs              | Done     | `StructArray.New` / `At` / `Free` |
| Methods on structs             | Done     | `fn Type.method(...)` + `obj.method(...)` desugar |
| Freestanding/UEFI backends     | Partial  | defs accepted; full field/method lowering still hosted |

These are intentional scope boundaries for Milestone 1.7. The goal was to give developers a solid, usable aggregate type so that real programs can start organizing data cleanly. Field order currently determines layout; there is no packing control yet.

## Migration from 1.6

Simply recompile. No source changes are required unless you want to start using structs.

## Update: Arrays of structs + Methods + status (completed)

- **Arrays of structs**: `StructArray.New(Type, count)`, `StructArray.At(base, index, Type)`, `StructArray.Free`.
- **Methods**: define with `fn TypeName.method(self, ...)`, call with `obj.method(...)`. The compiler desugars the call, passing the receiver (address or pointer) as the first argument.
- Parameter type annotations (`name: Type`) are now accepted on function parameters.
- Freestanding/UEFI: struct *definitions* are accepted by the hosted path; full field-offset and method lowering remain a hosted feature for this milestone (Partial).
- New example: `examples/structs_full_demo.purple` exercises stack, heap, arrays, nested structs, and methods.


## Update: Heap structs (`Struct.New`) (completed)

- `Struct.New(TypeName)` allocates a zero-filled instance of the named struct on the heap and returns a pointer.
- `Struct.Free(ptr)` / `Struct.Destroy(ptr)` release the memory (alias of `System.Memory.Free`).
- Field access and assignment through the pointer (`p.x = ...`, nested paths) are fully lowered.
- New example: `examples/heap_structs_demo.purple`.
- Fixed frame-size calculation so larger/stacked locals and multiple pointer locals no longer overflow the stack frame.


## Update: Full field-offset lowering (completed)

The codegen now:

- Registers all `struct` definitions and computes 8-byte-aligned layouts (including nested structs).
- Allocates the correct stack size for each struct local.
- Zero-fills the entire struct on `let s: StructName;`.
- Lowers both reads and writes of `obj.field` and `obj.nested.field` to the correct `offset(%rbp)` addressing.

Example of generated code for:

```purple
let p: Point;
p.x = 10;
p.y = 20;
```

```asm
movq $0, -64(%rbp)     # zero p.x
movq $0, -56(%rbp)     # zero p.y
movq $10, %rax
movq %rax, -64(%rbp)   # p.x = 10
movq $20, %rax
movq %rax, -56(%rbp)   # p.y = 20
```

Nested access (`hero.pos.x`) correctly accumulates offsets through the layout.
