# tzdata packages and templates

This directory contains the GN template that produces config data for packages
and underlying programs that need to load IANA timezone data as provided by
the [ICU](https://home.unicode.org) library.

# Provide ICU time zone resource files to the given package

The files `metaZones.res`, `timezoneTypes.res`, and `zoneinfo64.res` will be
made available in the namespace of the target component(s) at
`/config/data/tzdata/icu/{data_version}/{format}/`.

There will also be a file at `/config/data/tzdata/revision.txt` containing the
time zone database revision ID, e.g. `2019c`.

Options:
-  `for_pkg` (required) \
   [string] The name of the package to which time zone files will be supplied.

-  `data_version` (optional) \
   [string] The ICU version number of the time zone data. \
   Currently supported: { `"44"` } \
   Default: `"44"`

-  `format` (optional) \
   [string] The format name. \
   Currently supported: { `"le"` }. le = "Little-endian" \
   Default: `"le"`

In the future, this template may support newer versions or different formats
(e.g. `txt`).

Example:

```
icu_tzdata_config_data("icu_tzdata_for_web_runner") {
  for_pkg = "web_runner"
  data_version = "44"
  format = "le"
}
```

Additionally, `testonly` packages can be declared like so:

```
icu_tzdata_config_data("icu_tzdata_for_web_runner_test") {
  for_pkg = "web_runner_test"
  data_version = "44"
  format = "le"
  testonly = true
}
```

This injects an additional file `/config/data/FUCHSIA_IN_TREE_TEST`.

