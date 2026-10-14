"""Run PJSR scripts in a headless PixInsight, as a separate instance (slot 7) so that a GUI session that is already open is left alone.

Known requirements (found 2026-10-14, see the memory note on headless PixInsight):
  * /usr/bin/PixInsight is the launcher (the binary under /opt needs its libraries on the path); xvfb-run provides a display;
  * without -n=<slot> a run yields to an already running instance and does nothing;
  * the Gaia DR3/SP database paths used by SpectrophotometricColorCalibration live in the settings of slot 1, so they are copied to the slot used here;
  * script errors are silent, so every script body is wrapped to catch the exception and to write the console log next to the output;
  * /tmp is a small tmpfs: all work files go to the work directory given by the caller.
"""
import fcntl, os, shutil, subprocess, time

PIXINSIGHT = '/usr/bin/PixInsight'
SLOT = 7
SETTINGS_DIR = os.path.expanduser('~/.PixInsight')

WRAP = '''var __out = "";
Console.beginLog();
try {
%(body)s
} catch (e) { __out += "ERR " + e + "\\n"; }
__out += "\\n--- console ---\\n" + Console.endLog();
File.writeTextFile("%(log)s", __out);
'''


def ensure_settings():
    """copy the settings (database paths) of the interactive instance to the headless slot; the interactive instance's own file is only read"""
    src = os.path.join(SETTINGS_DIR, 'core-001-pxi.settings'); dst = os.path.join(SETTINGS_DIR, 'core-%03d-pxi.settings' % SLOT)
    if os.path.exists(src) and (not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst) + 3600):
        shutil.copyfile(src, dst)


def run(body, workdir, name, timeout=1800):
    """Run a script body (PJSR statements; may append to __out) and return (ok, text) where text is what the script wrote to __out plus the console log."""
    ensure_settings(); os.makedirs(workdir, exist_ok=True)
    log = os.path.join(workdir, name + '.log'); script = os.path.join(workdir, name + '.js')
    if os.path.exists(log): os.remove(log)
    open(script, 'w').write(WRAP % {'body': body, 'log': log})
    t0 = time.time()
    # one headless run at a time per slot: a second launch would yield to the first (no log, no output) and look like a failure
    os.makedirs(os.path.expanduser('~/.cache'), exist_ok=True)
    with open(os.path.expanduser('~/.cache/pi_flow_slot%d.lock' % SLOT), 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        subprocess.run(['xvfb-run', '-a', PIXINSIGHT, '-n=%d' % SLOT, '--automation-mode', '--force-exit', '-r=' + script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    text = open(log).read() if os.path.exists(log) else ''
    if not text: text = 'ERR no log written: the PixInsight launch did not run the script (another instance in this slot?)'
    ok = bool(text) and '\nERR ' not in '\n' + text.split('--- console ---')[0] and '*** Error' not in text
    return ok, text, time.time() - t0
