Paired OnePlus13 GLES Mesa source
================================
Upstream Mesa26.2.5 tarball SHA256:
794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5

mesa-26.2.5-op13.patch reconstructs the59 modified/new text source files from
the user-supplied OnePlus13 GLESv3 archive against that exact upstream release.
SOURCE-MANIFEST.json verifies every changed file after patching. Generated
Python caches, patch rejections and the generated build stamp are excluded.
No firmware, Apple system files, binaries or private logs are included.

This consolidated patch includes Darwin DRM/OSMesa support, restored OSMesa26
frontends, Freedreno/Gen8 fixes, the shared GL API bridge and donor AIR source
required by its build graph. Runtime uses GLES; Metal remains disabled.
Apply this consolidated patch once through tools/prepare-op13-gles-mesa.py;
do not then reapply the historical tools/mesa patches to the same tree.

Origin: user-supplied OnePlus13-GLES-v3-Sources-20261028-191401.tar.7z.
Donor userland: iola1999/hackios-xnu-userland k90-port,
2051390a14a9d967d185bd8f294fc086bf29c8e8 (archive runtime source pin).
Donor kernel: dc49065f8f17c3683925270388eba128f28eab33.
The archive's recovered v3 source also includes documented concurrency fixes;
these hashes identify its provenance, not a claim that one upstream checkout
alone reproduces the phone-tested combined port.

Mesa and added source retain their existing per-file licenses/notices. Mesa's
full unchanged source and license material come from the pinned upstream tar.
The existing HackiOS userland MIT license remains under that subtree.
