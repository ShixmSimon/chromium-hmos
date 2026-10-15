# Procedural executor

procedural_filters.ts is unmodified from brave/brave-core commit
076ea140325075a9f220d78daf9f588363d9d003, path
components/cosmetic_filters/resources/data/procedural_filters.ts (MPL-2.0).
procedural_filters.js is its type-stripped distribution with the module exports
removed for embedding. Node 24's node:module.stripTypeScriptTypes({mode:'strip'})
followed by removing exports and trailing whitespace reproduces the transformation; keep source and generated JS together.

controller.js adapts scheduling/action dispatch from Brave content_cosmetic.ts
at that commit. Lumie supplies a separate isolated world, native user styles,
DocumentToken ownership, stop/restart and BFCache/page visibility handling.
The 4 ms time slice yields between rules (maximum 16 per task); an individual
DOM selector/regexp is synchronous and cannot be preempted. At most 1024 rules
and 20000 matches per rule are processed. Device high-mutation performance
acceptance is still required. No errors include selectors or script arguments.
