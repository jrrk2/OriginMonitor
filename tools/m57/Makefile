# M57 (Ring Nebula) from the raw Origin frames of 2026-10-15: select -> register -> CFA drizzle 2x -> measured PSF -> Richardson-Lucy, plus a split-half check that the extra detail is
# in two independent stacks and not invented. Every step reads the FITS files in frames.txt; `make -n` shows the commands, `make verify` prints the evidence.
#
#   make all            everything (about 8 minutes on 12 cores)
#   make frames         clear 20 s frames from FROM to TO in the sequencer log (verdict clear, stacked)
#   make register       similarity transform of every frame to the middle frame, from matched stars
#   make drizzle        2x CFA drizzle of all frames; drizzle-odd / drizzle-even for the split test
#   make deconv         PSF from the stack's own isolated stars, RL, before/after images, star widths
#   make split          odd/even stacks deconvolved separately; correlation of the detail gained
#   make verify         inputs (count and SHA-256 of the file list), registration quality, star widths, split-half result
#   make clean          remove OUT
SHELL := /bin/bash
HERE  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PY    ?= python3 -I
SEQ   ?= /mnt/fits-backup/claude/sequence_20261015.jsonl
FROM  ?= 2026-10-15T20:48:48
TO    ?= 2026-10-15T22:16:00
ITERS ?= 28
OUT   ?= /mnt/fits-backup/claude/archive_qc/m57_pipeline
M57   := $(PY) $(HERE)m57.py

.PHONY: all frames register drizzle drizzle-odd drizzle-even deconv split verify clean
all: verify

frames: $(OUT)/frames.txt
$(OUT)/frames.txt: $(SEQ)
	mkdir -p $(OUT)
	$(M57) frames --seq $(SEQ) --from $(FROM) --to $(TO) --out $@

register: $(OUT)/reg.pkl
$(OUT)/reg.pkl: $(OUT)/frames.txt $(HERE)m57.py
	$(M57) register --list $< --out $@ | tee $(OUT)/register.log

drizzle: $(OUT)/drz_all.npy
drizzle-odd: $(OUT)/drz_odd.npy
drizzle-even: $(OUT)/drz_even.npy
$(OUT)/drz_%.npy: $(OUT)/reg.pkl
	$(M57) drizzle --list $(OUT)/frames.txt --reg $< --subset $* --out $@

deconv: $(OUT)/deconv/deconv.json
$(OUT)/deconv/deconv.json: $(OUT)/drz_all.npy
	$(M57) deconv --drz $< --iters $(ITERS) --out $(OUT)/deconv

split: $(OUT)/split.json
$(OUT)/split.json: $(OUT)/drz_odd.npy $(OUT)/drz_even.npy
	$(M57) split --odd $(OUT)/drz_odd.npy --even $(OUT)/drz_even.npy --iters $(ITERS) --out $@

verify: $(OUT)/deconv/deconv.json $(OUT)/split.json
	@echo "== inputs"; wc -l < $(OUT)/frames.txt | sed 's/$$/ raw frames (FITS, 20 s, ISO 800)/'; sha256sum $(OUT)/frames.txt | cut -c1-16
	@echo "== registration"; cat $(OUT)/register.log
	@echo "== PSF and star widths (output px = 0.739 arcsec)"; cat $(OUT)/deconv/deconv.json
	@echo "== split-half (odd vs even frames, independent stacks)"; cat $(OUT)/split.json

clean:
	rm -rf $(OUT)
