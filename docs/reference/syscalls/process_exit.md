# zx_process_exit

## NAME

<!-- Contents of this heading updated by update-docs-from-fidl, do not edit. -->

Exits the currently running process.

## SYNOPSIS

<!-- Contents of this heading updated by update-docs-from-fidl, do not edit. -->

```c
#include <zircon/syscalls.h>

[[noreturn]] void zx_process_exit(int64_t retcode);
```

## DESCRIPTION

The `zx_process_exit()` call ends the calling process with the given
return code. The return code of a process can be queried via the
**ZX_INFO_PROCESS** request to [`zx_object_get_info()`].

## RIGHTS

<!-- Contents of this heading updated by update-docs-from-fidl, do not edit. -->

None.

## RETURN VALUE

`zx_process_exit()` does not return.

## ERRORS

`zx_process_exit()` cannot fail.

## SEE ALSO

 - [`zx_object_get_info()`]
 - [`zx_process_create()`]

<!-- References updated by update-docs-from-fidl, do not edit. -->

[`zx_object_get_info()`]: object_get_info.md
[`zx_process_create()`]: process_create.md
