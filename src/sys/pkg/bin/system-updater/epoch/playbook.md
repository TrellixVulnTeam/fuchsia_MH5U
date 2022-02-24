# Playbook for updating the epoch

This document describes the playbook for updating the epoch (AKA the OTA backstop).

## Step 1: Decide if you should bump the epoch

First, you should decide if it's appropriate to bump the epoch. You should bump the epoch if your
change would prevent devices from successfully OTAing backwards across the change. For example, you
may want to bump the epoch if you're making a filesystem change that's incompatible with an old
driver.

The epoch should be bumped one-off as needed. The vast majority of changes should not require
epoch bumps.

## Step 2: File a public issue to track

Once you've decided on bumping the epoch, file a public issue on bugs.fuchsia.dev to describe why
the bump is necessary. Consider using fxbug.dev/72370 as an example.

## Step 3: Stage a CL

You should bump the epoch in the same CL that introduces the backwards-incompatible change.

Create a CL that:
* introduces the backwards-incompatible change.
* adds another line of `<epoch>=<context>` to
  [//src/sys/pkg/bin/system-updater/epoch/history](/src/sys/pkg/bin/system-updater/epoch/history),
  where `epoch` is the epoch you're bumping to and `context` is the link to the bug.
* updates `SOURCE_EPOCH` in
  [//src/sys/pkg/tests/system-updater/src/lib.rs](/src/sys/pkg/tests/system-updater/src/lib.rs)
  to match the new epoch.

## Step 4: Send your CL for approval and submit

Now that everything is in place, you can send your CL to anyone from
[//src/sys/pkg/OWNERS](/src/sys/pkg/OWNERS) for approval. When your CL is approved...congrats!
You can submit the change and officially bump the OTA backstop.

## Step 5: Share news widely

Send an email using this template:

```
To: announce@fuchsia.dev
Cc: pkg-dev@fuchsia.dev
Subject: Bumping the OTA backstop
Content: Following the playbook at https://cs.opensource.google/fuchsia/fuchsia/+/main:src/sys/pkg/bin/system-updater/epoch/playbook.md, I wanted to share I bumped the OTA backstop. This means you won't be able to OTA backwards across {link}. If you try and OTA backwards across this change, the OTA will fail and you may observe a log like: "downgrades from epoch {src} to {target} are not allowed." If you need to downgrade a device across this change, consider flashing. For more details, please see [RFC-0071](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0071_ota_backstop).
```

where `link` is a link to the CL, `src` is the epoch you bumped to, and `target` is a lower epoch.