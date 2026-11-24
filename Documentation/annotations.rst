Annotations
===========

Sparse extends C's type system with a number of extra type qualifiers
which add restrictions on what you can do on objects annotated with them.
These qualifiers are specified with GCC's ``__attribute__`` syntax.

address_space(*name*)
---------------------
This attribute is to be used on pointers to specify that its target is
in address space *name* (an identifier or a constant integer).

Sparse treats pointers with different address spaces as distinct types
and will warn on casts (implicit or explicit) mixing the address spaces.
An exception to this is when the destination type is ``uintptr_t`` or
``unsigned long`` since the resulting integer value is independent
of the address space and can't be dereferenced without first casting
it back to a pointer type.

bitwise
-------
This attribute is to be used to define new, unique integer types that
cannot be mixed with other types. In particular, you can't mix a
"bitwise" integer with a normal integer expression, and you can't even
mix it with another bitwise expression of a different type.
The integer 0 is special, though, and can be mixed with any bitwise type
since it's safe for all bitwise operations.

Since this qualifier defines new types, it only makes sense to use
it in typedefs, which effectively makes each of these typedefs
a single "bitwise class", incompatible with any other types.

context(*ctxt*, *entry*, *exit*)
--------------------------------
This attribute is to be used on function declarations to specify
the function's entry and exit count for a given context. This
context can be pretty much anything that can be counted.

Sparse will check that the function's entry and exit contexts match, and
that no path through a function is ever entered with conflicting contexts.
In particular, it is designed for doing things like matching up a "lock"
with the pairing "unlock". For example, a function doing a lock should be
annotated with an entry value of 0 and an exit value of 1, the corresponding
unlock function should use the values 1 and 0, and a function that should
only be called on some locked data, release the lock but which doesn't exit
without reacquiring the lock being, should use entry and exit values of 1.

The first argument, *ctxt*, is an expression only used as documentation
to identify the context. Usually, what is used is a pointer to the structure
containing the context, for example, the structure protected by the lock.

The default ``-Wcontext`` check keeps a single anonymous counter: all
context attributes, including the anonymous two-argument
``context(entry, exit)`` form and standalone ``__context__(delta)``
statements, contribute to the same total. A function that acquires a
reference on one object and releases it on a different object can
therefore balance even though neither object's obligation is met.

``-Wcontext-identity``
~~~~~~~~~~~~~~~~~~~~~~
The optional ``-Wcontext-identity`` warning turns the first argument of
a named ``context(ctxt, entry, exit)`` attribute into a real resource
identity and keeps a separate reference ledger for every object. Taking
a reference on one resource can then never repay an obligation owed on
another one. The analysis is purely static; the analyzed code is never
executed.

A resource identity can be:

* a pointer parameter of the enclosing function,
* the address of a named object (a global, static or local variable),
* a direct copy of such a pointer; casts and plain assignments are
  transparent to the identity. The spelling of a variable is never used
  to establish an identity.

When an annotated function is called, the parameter expression in its
declaration is substituted with the identity of the actual argument at
the call site. All contexts belonging to the same call are first
substituted and aggregated per resulting identity, then the entry
counts are verified, and only afterwards are the net changes applied.
Consequently the result does not depend on the order of the attributes,
and a call whose net change is zero (for example
``context(from,1,0), context(to,0,1)`` with both parameters naming the
same object) still requires the entry reference to be held.

Every function entry and exit, every early return and every control flow
join is checked per resource. Balanced paths produce no warning, while
distinct resources' gaps are reported separately and can never cancel.
Branches carrying the same identity still merge correctly. When an
identity cannot be determined — a join of different sources, an indirect
call or an unsupported address expression — sparse says so, keeps the
known ledgers and continues checking them instead of guessing an object
or discarding the state.

The anonymous two-argument ``context(entry, exit)`` attributes and
``__context__(delta)`` statements without a name continue to use the
independent anonymous counter and never repay a named resource. A named
``__context__(ctxt, delta)`` statement adjusts the ledger of the named
resource instead.

The values used only to name a resource are wired into the normal IR use
lists, so optimization cannot remove an expression whose only use is a
context annotation.

When the new warning is disabled, the original ``-Wcontext`` diagnostics
are unchanged.

See also https://lwn.net/Articles/109066/.

noderef
-------
This attribute is to be used on a r-value to specify it cannot be
dereferenced. A pointer so annotated is in all other aspects exactly
like a pointer  but trying to actually access anything through it will
cause a warning.

nocast
------
This attribute is similar to ``bitwise`` but in a much weaker form.
It warns about explicit or implicit casting to different types.
However, it doesn't warn about the mixing with other types and it easily
gets lost: you can add plain integers to __nocast integer types and the
result will be plain integers.
So, it ends to be more useful for big integers that still need to act
like integers, but you want to make it much less likely that they get
truncated by mistake. For example, a 64-bit integer that you don't want
to mistakenly/silently be returned as int.

See also `Linus' e-mail about __nocast vs __bitwise
<https://lore.kernel.org/linux-mm/CA+55aFzbhYvw7Am9EYgatpjTknBFm9eq+3jBWQHkSCUpnb3HRQ@mail.gmail.com/>`_.

safe
----
This attribute specifies that the object, which should be a pointer,
is defined to never be NULL or nontrapping.
It causes a warning if the object is tested in a conditional.

force
-----
This attribute is to be used in casts to suppress warnings that would
otherwise be caused by the presence of one of these extra qualifiers.
