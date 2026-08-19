# Style-checker fixtures

These four files exist to test `scripts/style-check.py`, not to be compiled.
`bad.cpp` and `bad.h` violate a listed set of rules on purpose; `good.cpp` and
`good.h` must produce **zero** findings.

```sh
python3 scripts/style-check.py --self-test
```

The expected findings per fixture live in `SELF_TEST_EXPECTED` in the checker.
**Adding a rule means adding a violation here and listing it there** —
otherwise the rule ships having never matched anything, and on a codebase this
clean that is indistinguishable from a rule that works.

Two rules cannot live in a committed fixture. `.gitattributes` pins `*.cpp`
and `*.h` to `eol=lf`, so git would repair a CRLF fixture on checkout and a
missing final newline is not representable at all. `eol-crlf` and
`no-final-eol` are therefore driven from `SELF_TEST_BYTES` in the checker,
which writes scratch files to a temporary directory at self-test time.

`good.cpp` is also doing double duty as a positive example: it carries the
space-indented `/*! ... */` doxygen continuation lines this tree uses above
every public method, which is the shape that made a naive
"indentation must be tabs" rule fire 499 times. If that rule ever regresses,
`good.cpp` catches it.

A normal `style-check.py` run skips this directory. It would always fail.
