# Fuchsia Networking Contributor Guide

Fuchsia Networking welcomes contributions from all. This document defines
contribution guidelines where they differ from or refine on the guidelines that
apply to Fuchsia as a whole.

## Getting Started

Consult the [getting started document][getting_started] to set up your
development environment.

## Contributor Workflow

Consult the [contribute changes document][contribute_changes] for general
contribution guidance and project-wide best practices. The remainder of this
document describes best practices specific to Fuchsia Networking.

## Coding Guidelines

### Philosophy

This section is inspired by [Flutter's style guide][flutter_philosophy], which
contains many general principles that you should apply to all your programming
work. Read it. The below calls out specific aspects that we feel are
particularly important.

#### Be Lazy

Do not implement features you don't need. It is hard to correctly design unused
code. This is closely related to the commit sizing advice given above; adding a
new data structure to be used in some future commit is akin to adding a feature
you don't need - it is exceedingly hard for your code reviewer to determine if
you've designed the structure correctly because they (and you!) can't see how it
is to be used.

#### Go Down the Rabbit Hole

You will occasionally encounter behaviour that surprises you or seems wrong. It
probably is! Invest the time to find the root cause - you will either learn
something, or fix something, and both are worth your time. Do not work around
behaviour you don't understand.

### Avoid Duplication

Avoid duplicating code whenever possible. In cases where existing code is not
exposed in a manner suitable to your needs, prefer to extract the necessary
parts into a common dependency.

### Error Handling

Avoid unhandled errors and APIs which inherently disallow proper error handling;
for a common example, consider [`fuchsia_async::executor::spawn`][spawn].
`spawn` inherently precludes error passing (since the flow of execution is
severed). In most cases `spawn` can be replaced with a future that is later
included in a [`select`][select] expression ([example commit][spawn_select]) or
simply `await`ed on directly ([example commit][spawn_await]).

### Compile-time over Run-time

Prefer type safety over runtime invariant checking. In other words, arrange your
abstractions such that they cannot express invalid conditions rather than
relying on assertions at runtime.

Write testable code; testable code is modular and its dependencies are easily
injected.

Avoid [magic numbers][magic_number].

### Comments

When writing comments, take a moment to consider the future reader of your
comment. Ensure that your comments are complete sentences with proper grammar
and punctuation. Note that adding more comments or more verbose comments is not
always better; for example, avoid comments that repeat the code they're anchored
on.

Documentation comments should be self-contained; in other words, do not assume
that the reader is aware of documentation in adjacent files or on adjacent
structures. Avoid documentation comments on types which describe _instances_ of
the type; for example, `AddressSet is a set of client addresses.` is a comment
that describes a field of type `AddressSet`, but the type may be used to hold
any kind of `Address`, not just a client's.

Phrase your comments to avoid references that might become stale; for example:
do not mention a variable or type by name when possible (certain doc comments
are necessary exceptions). Also avoid references to past or future versions of
or past or future work surrounding the item being documented; explain things
from first principles rather than making external references (including past
revisions).

When writing TODOs:

1. Include an issue reference using the format `TODO(https://fxbug.dev/1245):`
1. Phrase the text as an action that is to be taken; it should be possible for
   another contributor to pick up the TODO without consulting any external
   sources, including the referenced issue.

#### Provide citations

When an implementation is following some specification/document (e.g. RFCs),
include a comment with both a quote and citation of the relevant portion(s) of
the document near the implementation. The quote lets readers know why something
is being done and the citation allows a reader to get more context.

### Error Messages

As with code comments, consider the future reader of the error messages emitted
by your code. Ensure that your error messages are actionable. For example, avoid
test failure messages such as "unexpected value" - always include the unexpected
value; another example is "expected `<variable>` to be empty, was non-empty" -
this message would be much more useful if it included the unexpected elements.

Always consider: what will the reader do with this message?

### Tests

Consult the [testability rubrics][testability_rubrics] for general guidelines on
test-writing and testability reviews on Fuchsia. In Fuchsia Networking, we
define the following test classes:

- **Unit tests** are fully local to a piece of code and all their external
  dependencies are faked or mocked.
- **Integration tests** validate behavior between two or more different
  components.
- **End-to-end tests** are driven by an external host machine and use the public
  APIs and bytes written to the network to perform behavior validation. Can be
  performed over a physical network or by virtualization of the DUT (`qemu`).

Consider the following guidelines when writing tests:

1. **Always add tests** for new features or bug fixes.
1. Consider the guidelines in [Error Messages](#Error-Messages) when writing
   test assertions.
1. Tests must be **deterministic**. Threaded or time-dependent code, Random
   Number Generators (RNGs), and cross-component communication are common
   sources of nondeterminism.
     + **Don't use `sleep`** in tests as a means of weak synchronization. Only
       `sleep` when strictly necessary (e.g. when polling is required).
     + Time-dependent tests can use **fake or mocked clocks** to provide
       determinism. See [`fuchsia_async::Executor::new_with_fake_time`] and
       [fake-clock].
     + Threaded code must always use the proper synchronization primitives to
       avoid flakes. Whenever possible, prefer single-threaded tests.
     + Always provide a mechanism to **inject seeds for RNGs** and use them in
       tests.
     + Test for flakes locally whenever possible; use repeat flags in test
       binaries ([`--test.count`][go_test_flags] in Go,
       [`--gtest_repeat`][gtest_test_flags] for googletest) and aim for at least
       100-1000 runs locally if your test is prone to flakes before merging.
         > Rust test binaries currently don't have an equivalent flag, you may
         need to resort to a bash loop or equivalent to get repeated runs. See
         [#65218][rust_65218].
1. **Avoid** tests with **hard-coded timeouts**. Prefer relying on the
   framework/fixture to time out tests.
1. Prefer **hermetic tests**; test set-up routines should be explicit and
   deterministic. Be mindful of test fixtures that run cases in parallel (such
   as Rust's) when using "ambient" services. Prefer to **explicitly inject
   component dependencies** that are vital to the test.
1. [Tests should always be components][tests_as_components].
1. Prefer **virtual devices and networks** for non-end-to-end tests. See
   [netemul] for guidance on virtual network environments.
1. Avoid [change detector tests][change_detector_tests]; tests that are
   unnecessarily sensitive to changes, especially ones external to the code
   under test, can hamper feature development and refactoring.
1. Do not encode implementation details in tests, prefer testing through a
   module's public API.
1. When unwrapping a `Result<_, fidl::Error>` returned from a FIDL method call,
   restate the function being called in the panic message to make it easier to
   track down the callsite. Don't repeat the type of the error, which is already
   included in the panic output. For example:

   ```rust
   // Bad:
   let foo_result = proxy
       .foo() // `foo` returns a `Result<_, fidl::Error>`.
       .await
       .expect("FIDL error"); // Doesn't provide any new information.

   // Good:
   let foo_result = proxy
       .foo() // `foo` returns a `Result<_, fidl::Error>`.
       .await
       .expect("calling foo"); // Restate the function being called.
   ```

### Source Control Best Practices

Commits should be arranged for ease of reading; that is, incidental changes
such as code movement or formatting changes should be committed separately from
actual code changes.

Commits should always be focused. For example, a commit could add a feature,
fix a bug, or refactor code, but not a mixture.

Commits should be thoughtfully sized; avoid overly large or complex commits
which can be logically separated, but also avoid overly separated commits that
require code reviews to load multiple commits into their mental working memory
in order to properly understand how the various pieces fit together. **If your
changes require multiple commits, consider whether those changes warrant a
design doc or [RFC][rfc_process]**.

#### Commit Messages

Commit messages should be _concise_ but self-contained (avoid relying on issue
references as explanations for changes) and written such that they are helpful
to people reading in the future (include rationale and any necessary context).

Avoid superfluous details or narrative.

Commit messages should consist of a brief subject line and a separate
explanatory paragraph in accordance with the following:

1. [Separate subject from body with a blank line](https://chris.beams.io/posts/git-commit/#separate)
1. [Limit the subject line to 50 characters](https://chris.beams.io/posts/git-commit/#limit-50)
1. [Capitalize the subject line](https://chris.beams.io/posts/git-commit/#capitalize)
1. [Do not end the subject line with a period](https://chris.beams.io/posts/git-commit/#end)
1. [Use the imperative mood in the subject line](https://chris.beams.io/posts/git-commit/#imperative)
1. [Wrap the body at 72 characters](https://chris.beams.io/posts/git-commit/#wrap-72)
1. [Use the body to explain what and why vs. how](https://chris.beams.io/posts/git-commit/#why-not-how)

The body may be omitted if the subject is self-explanatory; e.g. when fixing a
typo. The git book contains a [Commit Guidelines][commit_guidelines] section
with much of the same advice, and the list above is part of a [blog
post](https://chris.beams.io/posts/git-commit/) by [Chris
Beams](https://chris.beams.io/).

Commit messages should make use of issue tracker integration. See [Commit-log
message integration][commit_log-message-integration] in the monorail
documentation.

When using issue tracker integration, don't omit necessary context that may
also be included in the relevant issue (see "Commit messages should be
_concise_ but self-contained" above). Many issues are Google-internal, and any
given issue tracker is not guaranteed to be usable at the time that the commit
history is read.

Commit messages should never contain references to any of:

1. Relative moments in time
1. Non-public URLs
1. Individuals
1. Hosted code reviews (such as on fuchsia-review.googlesource.com)
    + Refer to commits in this repository by their SHA-1 hash
    + Refer to commits in other repositories by public web address (such as
      https://fuchsia.googlesource.com/fuchsia/+/67fec6d)
1. Other entities which may not make sense to arbitrary future readers

Adding a `Test:` line to the commit message is encouraged. A `Test:` line
should:

1. Justify that any behavior changes or additions are thoroughly tested.
1. Describe how to run new/affected test cases.

For example: ``Test: Added new unit tests. `fx test netstack-gotests` ``.

## Code Review Guidelines

### Code Review Flow

The following code review guidelines are adopted within the Netstack team:

1. When your CL is ready for review, first request review from any team member
not part of the leads group.
1. Put leads in CC on the CL so they can follow any ongoing discussion and build
context.
1. You may prefer teammates that are more familiar with that area of the code,
but that's not mandatory. One of the goals is to spread out.
1. Once the initial reviewer approves your CL, the CC'ed lead commits to
assigning themselves as a reviewer and sending out comments and/or approving
within **two working hours**.

Be mindful that this scheme can increase latency for reviews, which is a side
effect we'd like to minimize. Try to decrease your latency when asked for review
and when addressing comments as well. Gerrit notification settings and smart
e-mail filters can be a big help to drive those interrupts. Also, don't be
afraid to ping for reviews.

## Tips & Tricks

### `fx set`

Run the following command to build all tests and their dependencies:

```
fx set core.x64 --with //src/connectivity/network:tests
```

If you're working on changes that affect `fdio` and `third_party/go`, add:

```
--with //sdk/lib/fdio:tests --with //third_party/go:go_stdlib_tests
```

[getting_started]: /docs/get-started
[contribute_changes]: /docs/development/source_code/contribute_changes.md
[spawn]: https://fuchsia.googlesource.com/fuchsia/+/a874276/src/lib/fuchsia-async/src/executor.rs#30
[select]: https://docs.rs/futures/0.3.4/futures/macro.select.html
[spawn_select]: https://fuchsia.googlesource.com/fuchsia/+/0c00fd3%5E%21/#F3
[spawn_await]: https://fuchsia.googlesource.com/fuchsia/+/038d2b9%5E%21/#F0
[magic_number]: https://en.wikipedia.org/wiki/Magic_number_(programming)
[rfc_process]: /docs/contribute/governance/rfcs/0001_rfc_process.md
[commit_guidelines]: https://www.git-scm.com/book/en/v2/Distributed-Git-Contributing-to-a-Project#_commit_guidelines
[commit_log-message-integration]: https://chromium.googlesource.com/infra/infra/+/HEAD/appengine/monorail/doc/userguide/power-users.md#commit_log-message-integration
[flutter_philosophy]: https://github.com/flutter/flutter/wiki/Style-guide-for-Flutter-repo#philosophy
[testability_rubrics]: /docs/development/testing/testability_rubric.md
[tests_as_components]: /docs/development/testing/run_fuchsia_tests.md
[netemul]: /src/connectivity/network/testing/netemul/README.md
[change_detector_tests]: https://testing.googleblog.com/2015/01/testing-on-toilet-change-detector-tests.html
[rust_65218]: https://github.com/rust-lang/rust/issues/65218
[go_test_flags]: https://golang.org/cmd/go/#hdr-Testing_flags
[gtest_test_flags]: https://github.com/google/googletest/blob/HEAD/googletest/docs/advanced.md#repeating-the-tests
[`fuchsia_async::Executor::new_with_fake_time`]: https://fuchsia.googlesource.com/fuchsia/+/a874276/src/lib/fuchsia-async/src/executor.rs#345
[fake-clock]: https://fuchsia.googlesource.com/fuchsia/+/a874276/src/lib/fake-clock
