# Build passes

Source-to-source passes run over the translated code (`port/src/recomp/gen/`)
after the recompiler, in the order `port/tools/fork_regen.sh` lists them. Each
one fixes a class of translation that the lifter gets wrong for this game
(x87 branches, `rep` string ops, scalar SSE, split flags, indirect-call saves,
narrow sign extension, FPU returns and memory operands).

They are part of the build: `build.sh` runs them for you. Run one by hand from
this folder with `python <pass>.py --help`.
