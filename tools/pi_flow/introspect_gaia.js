var out = {};
["Gaia", "APASS"].forEach(function(n) {
  try { var cls = eval(n); var P = new cls; var props = {};
    for (var k in P) { try { var v = P[k]; if (typeof v != "function") props[k] = (typeof v == "object") ? JSON.stringify(v) : v; } catch (e) {} }
    var consts = {}; for (var k2 in cls.prototype) if (/^[A-Z]/.test(k2)) { try { consts[k2] = cls.prototype[k2]; } catch (e) {} }
    out[n] = {parameters: props, constants: consts}; } catch (e) { out[n] = {error: String(e)}; }
});
File.writeTextFile("/mnt/fits-backup/claude/archive_qc/pi_flow/gaia_process.json", JSON.stringify(out));
