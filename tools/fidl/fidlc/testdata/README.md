# FIDL test libraries

This directory contains the FIDL test libraries used for golden files.

## Add a new library

To add a new standalone library:

1. Choose a **name**, e.g. `foo_bar`.
2. Create a FIDL file with the `.test.fidl` extension, e.g. `foo_bar.test.fidl`.
3. Declare the library as `test.` followed by **name** without underscores,
   e.g. `library test.foobar;`.
4. Add the filename to the `standalone_libraries` list in BUILD.gn.
5. Add an entry to `fidl_testdata_info` in info.gni, providing the **name** and
   the target that BUILD.gn generates, e.g.:

```
{
  name = "foo_bar"
  target = "//tools/fidl/fidlc/testdata:test.foobar"
}
```

## Add a new library with dependencies

To add a new library with dependencies, you must create a subdirectory
containing multiple FIDL files. This should only be done to accommodate
dependencies, not to group together similar standalone libraries.

1. Choose a **name**, e.g. `foo_bar`.
2. Create a subdirectory named **name** containing two or more FIDL files ending
   in `.test.fidl` and a BUILD.gn to build them.
3. Ensure one of the libraries is named appropriately, e.g. `test.foobar`,
   and that its build target name is the same.
4. Add an entry to `fidl_testdata_info` in info.gni, providing the **name** and
   the target that BUILD.gn generates, e.g.:

```
{
  name = "foo_bar"
  target = "//tools/fidl/fidlc/testdata/foo_bar:test.foobar"
}
```

## Golden tests

FIDL tools in //tools/fidl use the test libraries as input when defining golden
tests with //build/testing/golden_test.gni.

To regenerate all goldens, run `fx regen-goldens fidl`.

To test goldens, run `fx test ${TOOL}_golden_tests` for the specific tool,
e.g. `fx test fidlc_golden_tests`.
