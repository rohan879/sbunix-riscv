# sbush Known Bugs and Limitations

This file documents bugs and limitations discovered in sbush during os-test integration work.

## Fixed Bugs (March 2026)

### 1. Variable assignment now expands `$VAR`, `$1`, `$(cmd)`, etc. ✓ FIXED

Previously:
```sh
x=$1           # x was literally "$1", not the value of $1
y=$HOME        # y was literally "$HOME"
```

Now works correctly - values are expanded before assignment.

**Fix:** Added call to `expand_vars()` in the assignment handler (`sbush.c:183`).

### 2. Variable assignment now works with `;` on same line ✓ FIXED

Previously:
```sh
x=hello; echo $x   # Error: "sbush: x=hello: command not found"
```

Now works correctly.

**Fix:** Added assignment handling in `expand_and_parse_pipeline()` (`parse.c:985-1028`) so that when `x=hello` becomes a separate segment after `;` splitting, it's properly recognized as an assignment.

### 3. Arithmetic expansion now works in variable assignment ✓ FIXED

Previously:
```sh
count=0
count=$((count + 1))   # Error: "sbush: count=1: command not found"
```

Now works correctly because `expand_vars()` is called on the value.

## Remaining Limitations (by design)

### No array support

Arrays like `arr=(a b c)` are not supported. This is expected for a minimal shell.

### Limited parameter expansion

Basic forms are supported: `${VAR}`, `${VAR:-default}`, `${VAR#pattern}`, `${VAR%pattern}`. More complex expansions may not work.

### No `local` keyword

Function-local variables aren't truly local - they modify the global scope.

## Implementation Notes

The fix involved two changes:

1. **`parse.c:463`**: Made `expand_vars()` non-static so it can be called from other files.

2. **`sbush.c:183`**: In the standalone assignment handler, added `expand_vars(val)` before `var_set(name, val)`.

3. **`parse.c:985-1028`**: Added assignment detection in `expand_and_parse_pipeline()` to handle `x=hello` when it's a segment by itself (after `;` splitting). This runs AFTER `expand_vars()` so the value is already expanded.
