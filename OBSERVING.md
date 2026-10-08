# Supervised observing: OriginMonitor measures, a Claude session decides

OriginMonitor (headless or GUI) never moves the telescope by itself. For every snapshot it analyses the image, adds usable frames to a live
per-target stack, and records telescope errors. A Claude session polls it through `originctl.py` (or the JSON control port, 127.0.0.1:8765)
and decides what to do: keep going, wait for cloud to pass, change target, refocus, park.

    CelestronOriginMonitor --headless --connect 192.168.1.195        # then, per night:
    ./originctl.py monitor --lat 52.205 --lon 0.122                   # observer site (the mount's stored GPS site is wrong)
    ./originctl.py target M51 --ra-hours 13.4979 --dec-deg 47.195     # chooses/resumes the live stack for this target

## What the monitor reports

`originctl.py analysis [--history N]` returns one record per snapshot:

| field | meaning |
|---|---|
| `verdict` | `clear`, `hazy`, `cloudy`, `patchy`, `moon_glow`, `twilight`, `off_target`, `unknown`, `unreadable` (a suggestion, built from the numbers below) |
| `reasons`, `notes` | why, in words (low altitude, moon glow, twilight, sky brighter than the dark baseline) |
| `registration.transparency` | star flux against the best this target has shown (1 = as clear as it gets); under 0.5 is cloud |
| `registration.tile_coverage` | 12 values (4x3 grid of the stack frame): fraction of the expected stars present; low tiles on one side = cloud edge or obstruction |
| `hfr_px`, `hfr_arcsec`, `stars`, `hfr_spread_px` | focus quality (lower is better); a clear-sky rise of 25-30% over the target's best means refocus |
| `sky_rate`, `sky_excess_over_dark`, `sky_gradient_sigma` | sky brightness per second, excess over the lowest dark-sky value ever seen at this ISO, and background tilt in noise units (moon halo, twilight, light dome) |
| `alt_deg`, `az_deg`, `moon_alt_deg`, `moon_separation_deg`, `moon_illumination`, `sun_alt_deg`, `off_target_deg` | geometry at the time of the frame |
| `sky_map_history` | how often frames at this az/alt were good or bad, and on how many nights |
| `stack` | frames and total exposure in the current target's stack |

`originctl.py events [--since SEQ]` is the telescope error feed (`telescope_error`, `telescope_alert`, `connection_error`, `disconnected`,
`download_error`, `image_unreadable`); remember `next` and pass it as `--since`. `originctl.py stack` saves the stack and returns the path of
`preview.png` (look at it). `originctl.py sky-map` lists az/alt bins that were bad on several nights and never good (almost certainly trees
or buildings) next to bins that are only sometimes bad (cloud).

## Telling the causes apart

* **Cloud:** transparency falls on every frame whatever the pointing, `sky_excess_over_dark` often rises, the same az/alt is good again later.
* **Trees / buildings:** `patchy` or `cloudy` at a low altitude and the same az bin bad on several nights while the rest of the sky is clear.
  Check `sky-map`; avoid that bin, or wait for the target to climb out of it.
* **Moon glow:** `moon_glow`, a high `sky_gradient_sigma` and `sky_excess_over_dark` with the moon up and a small `moon_separation_deg`. Stars are
  still measurable, so frames keep stacking but get less value; prefer targets far from the moon.
* **Twilight:** `sun_alt_deg` above about -12. Stop imaging for the night when it is above -12 and rising.
* **Wrong pointing:** `off_target` means the mount is not where `target` said; do not read it as cloud.

## Suggested policy for a supervising session

1. Poll `analysis` after each snapshot (every 10-15 s of exposure) and `events` on every poll. Any `telescope_error`/`disconnected`: stop
   and report to the user rather than retrying blindly.
2. Three consecutive `cloudy`/`patchy` frames on a target: treat it as blocked. Pick another target that is high, away from the moon and
   not in a bad sky-map bin, slew there, `target` it, and keep a note of when to come back (retry after 5, 10, 20, 30 minutes).
3. Returning: slew back, `target` the same name (the saved stack resumes), and take 2 frames; two `clear` frames mean carry on.
4. After every goto the firmware centres and autofocuses (about 50 s): wait for the mount's `IsGotoOver` and TaskController IDLE before judging
   a frame. Re-send exposure/ISO/16-bit settings afterwards (the telescope reverts them).
5. Focus: rely on the firmware autofocus after goto. If `hfr_px` stays 25-30% above the target's best for 8+ clear frames, repeat the goto
   (which retriggers centring and autofocus) at most once per 30 minutes.
6. Finish: when `sun_alt_deg` passes about -12, or all targets are blocked for hours, park and save (`stack`).

Slewing is a decision with consequences for the user's night: only do it when they have started an unattended session ("you have control"),
and state in the log what the telescope was pointing at and where it is going.
