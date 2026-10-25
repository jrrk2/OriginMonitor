# pi_flow: data-driven processing with a headless PixInsight (prototype)

Takes a stack (or the raw frames of a target) and chooses what to do to it from the target and from measured data conditions, runs the heavy steps in the installed PixInsight,
searches the parameters that cannot be ruled, and writes a before/after picture and a report that says why each step was taken.

    flow.py --stack STACK.fits [--swap-rb] --ref-header RAW_FRAME.fits --target "M 1" --nframes 142 --total-s 2438 --out DIR
    flow.py --frames FRAMES.txt --target "NGC 7000" --out DIR        # builds the stack first (Debayer GRBG, StarAlignment, ImageIntegration)

Files: `export_livestack.py` (live stack state to a linear RGB FITS, half resolution; use `flow.py --bin 2` on it), `pi_run.py` (headless PixInsight runner), `assess.py` (metrics, target classes), `recipe.py` (the rules, each with its reason), `flow.py` (orchestration), `introspect.js` (lists the
processes a script can create and their parameters; output in `archive_qc/pi_flow/process_catalog.json`).

## What is measured, and what it decides

| Measured from the stack | Used for |
|---|---|
| sky gradient beyond a plane (in noise units) | order of the polynomial background model (1 to 4) |
| empirical PSF from isolated stars: FWHM, ellipticity, star count | whether deconvolution is worth running; the PSF it uses |
| target-region signal-to-noise (smoothed signal over blank-sky noise) | deconvolution range, denoise target, saturation, local contrast |
| number of frames | integration rejection method |
| star density, clipped-star fraction | notes, and white-core handling |
| Gaia DR3 match of the stack (astrometry, own code) | the target's pixel position and the SPCC solution |

Target class (galaxy, emission or reflection nebula, planetary nebula, supernova remnant, open or globular cluster) and faintness come from a small table (`assess.TARGETS`) and set the stretch level,
colour saturation and the star-field handling.

Searched, not ruled (closed loop): **deconvolution iterations** (scored on core sharpening against ringing, noise growth and the sampling limit) and **TGV strength** (smallest strength reaching a
target noise ratio). Both log every candidate.

## Findings from the first two runs
* PixInsight's regularized Richardson-Lucy raised the sky level and the noise at low iteration counts on the M1 stack (8 iterations: noise x3.2, sky 0.0149; 25 iterations: x1.0, 0.0101), so a rule of "fewer iterations is safer" would have been wrong; the search handles it.
* TGV's stock strengths (5 / 0.002) flattened the linear stack to 4% of its noise and blotched the sky; with the edge protection tied to the measured noise (0.3 x sigma) strength 1.0 gives x0.55 and keeps the star cores.
* A core FWHM below about 1.8 px is under what a 1.478 arcsec pixel samples: the score penalises it (70 iterations had reached 1.43 px).
* Catalogue: the flow queries the **local PixInsight Gaia DR3 database** through the `Gaia` process (0.1 s search, about 5 s with the launch, offline, complete to G=16.6); the ESA TAP service is only a fallback (it timed out on the Cygnus field).
* **Registration must be verified.** PixInsight's StarAlignment reported "180 succeeded, 0 failed" for 180 frames of 4 s on NGC 7000 at altitude 81 degrees and left them essentially unregistered (only 9 to 34% of the bright stars coincide with the reference, no empty borders); the stack was star trails and the astrometry correctly found nothing. `check_registration` now exports five registered frames and stops the run when they do not line up; the live stacker registered the same frames, so its registration is the fallback.
* Astrometry from the raw header needs a wide, rotation-aware search: the pointing in the header can be off by 0.1 degree or more (the mount had just initialised), and the angle is less reliable near the zenith. `solve_wcs` votes over orientation (+-12 degrees, then the full circle with flips) and over offsets of +-0.74 degrees before refining an affine fit.
* Two flows must not share a PixInsight slot: the second launch yields to the first (no log, no output) and looks like a failure. `pi_run.run` takes a lock.
* The live stacker keeps weighted sums at half resolution (about 1528 x 1024, 2.96 arcsec/px). Rebuilding from the full-resolution snapshots (which are kept) gives the flow twice the sampling.
* Headless PixInsight facts (separate instance slot, settings copy, mirrored FITS for SPCC, no `/tmp`) are in `pi_run.py` and in the memory note.

## Licence boundary
PixInsight is driven only through its scripting interface and documentation; no PixInsight Class Library source (nor the bundled script sources) is read or derived here. The PCL licence (v2.0, 2025)
forbids use of the library source for automated code generation systems without written permission; see the discussion in the session notes.

## Not done yet
Star reduction, SCNR, GHS (no process by that name in this install; would be PixelMath or own code), a second recipe family for narrowband data, export of the live stack to FITS, a daemon command
that runs a job in the background and publishes the result to OriginWatch, and tuning the rules on more targets.
