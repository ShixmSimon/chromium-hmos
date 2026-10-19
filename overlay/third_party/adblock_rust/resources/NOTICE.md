# Offline filtering resources

This directory contains the exact resource JSON embedded in liblumie_adblock.so.
It is updated only with the application/HAR. Network subscriptions cannot replace
it. SOURCES.json records upstream revisions, integrity hashes and policy.

- Brave Authors: adblock-resources, MPL-2.0 (BRAVE-MPL-2.0.txt).
- Raymond Hill and uBlock Origin contributors: uBlock scripts/resources,
  GPL-3.0-or-later (UBLOCK-GPL-3.0.txt); individual resource headers may name
  additional authors and licenses.
- Original source files and copyright/license headers are in sources.tar.gz.
  The bundle is generated with scripts/assemble-adblock-resources.mjs, using
  the unpacked pinned Brave/uBlock trees. No Brave updater identity is included.
- Lumie changes: require explicit resource permissions, reject duplicate aliases
  and missing dependencies, freeze inputs, omit google-ima-dai and redirect
  resources requiring parameter substitution, embed and hash the final bundle.

Permission bit 0 protects uBO requiresTrust resources. Bit 1 protects Brave
resources. Custom subscriptions, file imports and personal rules get neither.
The reviewed offline brave-compat pair alone gets bits 0 and 1. Downloaded text
cannot acquire that permission, even when it has the same source ID.
