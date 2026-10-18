# M45, the Pleiades, night of 2026-10-13/14 (13 s, ISO 800, no filter): the 13 s block from 04:12 to 05:23 (the sequencer's clear and hazy frames). The cluster is about 2 degrees across,
# larger than the 1.2 x 0.84 degree field, so the window is the WHOLE frame (6112 x 4096 output px): use WORKERS=5 on a 32 GB machine (each drizzled frame is 1.4 GB in flight).
# OBJ = a disc (output px) round the frame centre for the split-half test.
NAME     ?= m45
SEQ      ?= /mnt/fits-backup/claude/sequence_20261014.jsonl
FROM     ?= 2026-10-14T04:12:00
TO       ?= 2026-10-14T05:23:30
VERDICTS ?= clear hazy
WIN      ?= 0 0 3056 2048
OBJ      ?= 3056 2048 700
CROP     ?=
ZOOM     ?= 1
