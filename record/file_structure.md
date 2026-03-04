# File Structure Snapshot

Updated: 2026-03-01

## Top-level

```text
CHANGELOG.md
CMakeLists.txt
README.md
bench
compiler
doc
docs -> doc
k
record
runtime
scripts
stdlib
src
test
tests -> test
tune
```

## Source architecture hierarchy

```text
src/
  application/
  port/{input,output}
  core/
    dto/{atom,molecule,compound,tissue,organ,system,organism}
    behavior/{atom,molecule,compound,tissue,organ,system,organism}
    mapper/{atom,molecule,compound,tissue,organ,system,organism}
    driver/{atom,molecule,compound,tissue,organ,system,organism}
    manager/{atom,molecule,compound,tissue,organ,system,organism}
    servis
    logic
    mode
    ui
    common
```

## Compiler source layout (key)

```text
compiler/src/
  application/wiring
  port/{input,output}
  core/{common,behavior,mapper,driver,manager,servis,logic,mode,ui}
  phase1..phase9
  common
  container
  runtime
```

## Records and docs

```text
record/bench_results
bench/results -> ../record/bench_results
doc/
docs -> doc
```
