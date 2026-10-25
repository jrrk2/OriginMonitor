# M13 (NGC 6205), night of 2026-10-11, 20 s ISO 800, no filter. The 31 clear frames from 20:27 to 20:57 local (the sequencer's verdict; cloud ended the block). The cluster is at frame pixel (1550, 1020),
# so WIN is an 820 x 1100 px window around it (starts right of the satellite trail's end). OBJ = a disc (output px) around the cluster for the split-half test.
NAME  ?= m13
SEQ   ?= /mnt/fits-backup/claude/sequence_20261011.jsonl
FROM  ?= 2026-10-11T20:27:00
TO    ?= 2026-10-11T21:12:00
WIN   ?= 1140 500 820 1100
OBJ   ?= 820 1040 200
CROP  ?=
ZOOM  ?= 1
