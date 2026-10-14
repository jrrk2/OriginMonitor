"""Choose processing steps and parameters from the target class and the measured data conditions.

Every decision carries its reason, so the report says why a step was taken, skipped or tuned. The numbers are starting points taken from common practice and from the M1 stack of 2026-10-13/14
(142 x 17 s, PSF 4.6 px, target SNR 86); they are meant to be tuned against more targets. The two choices that are searched, not ruled, are made in flow.py (deconvolution iterations).
"""

def choose(m, cls, size_arcmin, faint, obj_snr, nframes, total_s):
    plan = {'why': {}, 'steps': {}}
    why = plan['why']
    S = plan['steps']

    # --- integration (only used when starting from frames)
    if nframes < 8: rej, why['rejection'] = 'PercentileClip', 'fewer than 8 frames: percentile clipping'
    elif nframes < 20: rej, why['rejection'] = 'SigmaClip', '8 to 19 frames: sigma clipping'
    elif nframes < 60: rej, why['rejection'] = 'WinsorizedSigmaClip', '20 to 59 frames: Winsorized sigma clipping'
    else: rej, why['rejection'] = 'LinearFit', '60 or more frames: linear-fit clipping'
    S['integration'] = {'rejection': rej, 'sigmaLow': 4.0, 'sigmaHigh': 3.0}

    # --- background gradient: model order from how much more than a plane the sky needs
    g = m['gradient_rms_over_noise']
    deg = 1 if g < 0.5 else 2 if g < 2 else 3 if g < 6 else 4
    S['background'] = {'degree': deg}
    why['background'] = 'sky needs %.1f noise units beyond a plane: polynomial degree %d' % (g, deg)

    # --- deconvolution: worth it only for a resolved, round PSF and enough signal; the iteration count is searched later within the range given here
    fw, ell, nps = m['psf_fwhm_px'], m['psf_ellipticity'], m['psf_stars']
    if not (fw == fw) or nps < 150:
        S['deconvolution'] = None; why['deconvolution'] = 'skipped: only %s isolated stars to measure a PSF (need 150)' % nps
    elif ell > 0.15:
        S['deconvolution'] = None; why['deconvolution'] = 'skipped: PSF is not round (ellipticity %.2f): tracking or field-rotation blur, a symmetric PSF would ring' % ell
    elif fw < 3.0:
        S['deconvolution'] = None; why['deconvolution'] = 'skipped: PSF already sharp (FWHM %.1f px)' % fw
    elif obj_snr == obj_snr and obj_snr < 3.0 and cls != 'open_cluster' and cls != 'globular_cluster':
        S['deconvolution'] = None; why['deconvolution'] = 'skipped: target signal-to-noise %.1f is too low, deconvolution would amplify noise' % obj_snr
    else:
        snr = obj_snr if obj_snr == obj_snr else 10.0
        its = [8, 12, 18, 25] if snr >= 40 else [6, 10, 15, 20] if snr >= 10 else [4, 6, 9, 12]
        S['deconvolution'] = {'iterations_to_try': its, 'dering_dark': 0.03, 'regularization': True, 'min_core_fwhm_px': 1.8}
        why['deconvolution'] = 'PSF FWHM %.1f px round (ellipticity %.2f), target SNR %.0f: try %s iterations, scored on sharpening against ringing, noise growth and the sampling limit (core FWHM under 1.8 px is below what a %.2f arcsec pixel can sample)' % (fw, ell, snr, its, 1.478)

    # --- denoise (TGV on the linear data): edge protection follows the measured noise; the strength is searched until the noise has fallen to a target ratio, higher for bright targets (detail first)
    snr = obj_snr if obj_snr == obj_snr else 10.0
    target = 0.65 if snr >= 40 else 0.55 if snr >= 10 else 0.45
    S['denoise'] = {'edge_over_noise': 0.3, 'strengths_to_try': [0.5, 1.0, 2.0, 3.0], 'target_noise_ratio': target}
    why['denoise'] = 'target SNR %.0f: TGV edge protection 0.3 x noise, smallest strength of %s that brings the noise to %.2f of its value (a probe showed strength 1.0 / edge 0.0005 gives x0.55 here; the stock 5 / 0.002 gave x0.04, a blotchy sky)' % (snr, [0.5, 1.0, 2.0, 3.0], target)

    # --- stretch: put the target's typical brightness at a class-dependent display level (arcsinh, colour preserving)
    star_field = cls in ('open_cluster', 'globular_cluster')
    extended = size_arcmin >= 60 and not star_field        # a target as large as a third of the 75 x 50 arcmin frame or more: its bright core must not set the display level
    level = 0.12 if star_field else (0.40 if faint < 0.3 else 0.33 if faint < 0.7 else 0.27)
    if extended: level = 0.30
    ref = 'sky background' if star_field else ('extended signal' if extended else 'target region')
    S['stretch'] = {'target_display_level': level, 'black_sigma': 1.0 if not extended else 0.5, 'reference': ref}
    why['stretch'] = '%s, %.0f arcmin, faintness %.1f: display level %.2f for the %s' % (cls, size_arcmin, faint, level, {'sky background': 'sky (the stars are the subject)', 'extended signal': '85th percentile of the smoothed signal (the target fills the frame: the faint outer parts, not the core, set the stretch)', 'target region': 'target region'}[ref])

    # --- colour saturation and chroma noise
    base = {'emission_nebula': 2.2, 'snr': 2.2, 'planetary_nebula': 2.0, 'reflection_nebula': 2.0, 'galaxy': 1.8, 'open_cluster': 1.6, 'globular_cluster': 1.6}.get(cls, 1.8)
    sat = base * (0.75 if snr < 8 else 1.0)
    S['saturation'] = sat; why['saturation'] = '%s base %.1f%s' % (cls, base, ', reduced 25% because the target SNR is under 8 (chroma noise)' if snr < 8 else '')

    # --- local contrast ("clarity") on the stretched luminance
    clar = 0.45 if snr >= 20 else 0.30 if snr >= 8 else 0.15
    S['clarity'] = clar; why['clarity'] = 'target SNR %.0f: local contrast amount %.2f' % (snr, clar)

    # --- notes on options that were considered but are not executed yet
    notes = []
    if m['star_density_per_mpx'] > 800 and cls in ('emission_nebula', 'snr', 'reflection_nebula'):
        notes.append('dense star field (%.0f per Mpx) over nebulosity: star reduction would help (not implemented)' % m['star_density_per_mpx'])
    if m['saturated_star_fraction'] > 0.25:
        notes.append('%.0f%% of detected stars have clipped cores: cores are blended to white in the render' % (100 * m['saturated_star_fraction']))
    plan['notes'] = notes
    return plan
