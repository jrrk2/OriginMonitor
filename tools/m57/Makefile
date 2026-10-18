# A target from the raw Origin frames: select -> register -> CFA drizzle 2x -> measured PSF -> Richardson-Lucy with clipped pixels as missing data, plus a split-half check that the extra
# detail is in two independent stacks and not invented. Every step reads the FITS files in frames.txt; `make -n` shows the commands, `make verify` prints the evidence.
#
#   make all TARGET=m57                  (default)     |   make all TARGET=double_cluster
#   A target is a preset in targets/<TARGET>.mk: NAME, SEQ (sequencer log), FROM/TO (clear frames of that block), WIN (window of the frames processed, px: X0 Y0 W H), CROP and ZOOM (pictures). Any of them
#   can also be given on the command line; to add a target copy a preset and change those.
#
#   TAIL=1   subtract the camera-fixed star-tail template (tail.py; built per target from its own frames) from every raw frame before registration: make all TARGET=double_cluster NAME=double_cluster_tail TAIL=1
#   make tail           build that template ($(OUT)/tail.npz)
#   make frames         clear frames from FROM to TO in the sequencer log (verdict clear, stacked)
#   make register       similarity transform of every frame to the middle frame, from matched stars
#   make drizzle        2x CFA drizzle of all frames (and a map of clipped samples); drizzle-odd / drizzle-even for the split test
#   make deconv         PSF from the stack's own isolated stars, masked RL, before/after pictures, star widths
#   make split          odd/even stacks deconvolved separately; correlation of the detail gained, against blank sky
#   make verify         inputs (count and SHA-256 of the file list), registration quality, star widths, split-half result
#   make clean          remove OUT
SHELL := /bin/bash
HERE  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PY    ?= python3 -I
TARGET ?= m57
include $(HERE)targets/$(TARGET).mk
ITERS ?= 28
OUT   ?= /mnt/fits-backup/claude/archive_qc/$(NAME)_pipeline
TAIL  ?=
WORKERS ?= 11
VERDICTS ?= clear
M57N  := $(PY) $(HERE)m57.py --window $(WIN) --workers $(WORKERS)
M57   := $(M57N) $(if $(TAIL),--tail $(OUT)/tail.npz)

tail: $(OUT)/tail.npz
$(OUT)/tail.npz: $(OUT)/frames.txt $(HERE)tail.py
	$(PY) $(HERE)tail.py build --list $< --out $@

.PHONY: tail all frames register drizzle drizzle-odd drizzle-even deconv split verify clean
all: verify

frames: $(OUT)/frames.txt
$(OUT)/frames.txt: $(SEQ)
	mkdir -p $(OUT)
	$(M57N) frames --seq $(SEQ) --from $(FROM) --to $(TO) --out $@ --verdicts $(VERDICTS)

register: $(OUT)/reg.pkl
$(OUT)/reg.pkl: $(OUT)/frames.txt $(HERE)m57.py $(if $(TAIL),$(OUT)/tail.npz)
	$(M57) register --list $< --out $@ | tee $(OUT)/register.log

drizzle: $(OUT)/drz_all.npy
drizzle-odd: $(OUT)/drz_odd.npy
drizzle-even: $(OUT)/drz_even.npy
$(OUT)/drz_%.npy: $(OUT)/reg.pkl $(HERE)m57.py $(if $(TAIL),$(OUT)/tail.npz)
	$(M57) drizzle --list $(OUT)/frames.txt --reg $< --subset $* --out $@

deconv: $(OUT)/deconv/deconv.json
$(OUT)/deconv/deconv.json: $(OUT)/drz_all.npy $(HERE)m57.py
	$(M57) deconv --drz $< --iters $(ITERS) --out $(OUT)/deconv --name $(NAME) $(if $(CROP),--crop $(CROP)) --zoom $(ZOOM)

split: $(OUT)/split.json
$(OUT)/split.json: $(OUT)/drz_odd.npy $(OUT)/drz_even.npy $(HERE)m57.py
	$(M57) split --odd $(OUT)/drz_odd.npy --even $(OUT)/drz_even.npy --iters $(ITERS) --out $@ $(if $(OBJ),--obj $(OBJ))

verify: $(OUT)/deconv/deconv.json $(OUT)/split.json
	@echo "== inputs"; wc -l < $(OUT)/frames.txt | sed 's/$$/ raw frames (FITS, 20 s, ISO 800)/'; sha256sum $(OUT)/frames.txt | cut -c1-16
	@echo "== registration"; cat $(OUT)/register.log
	@echo "== PSF and star widths (output px = 0.739 arcsec)"; cat $(OUT)/deconv/deconv.json
	@echo "== split-half (odd vs even frames, independent stacks)"; cat $(OUT)/split.json

clean:
	rm -rf $(OUT)
