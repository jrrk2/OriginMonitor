// Lists the processes this PixInsight can create from a script and the parameters each one exposes (runtime introspection of the installed product; writes JSON next to the script output path given in OUT)
var OUT = "/mnt/fits-backup/claude/archive_qc/pi_flow/process_catalog.json";
var names = ["AutomaticBackgroundExtractor","BackgroundNeutralization","ChannelCombination","ChannelExtraction","ColorCalibration","SpectrophotometricColorCalibration","ColorSaturation","CurvesTransformation",
 "Debayer","Deconvolution","Crop","DynamicCrop","GradientCorrection","HistogramTransformation","ImageCalibration","ImageIntegration","LocalHistogramEqualization","LocalNormalization",
 "MorphologicalTransformation","MultiscaleLinearTransform","MultiscaleMedianTransform","MultiscaleAdaptiveStretch","TGVDenoise","SCNR","SubframeSelector","StarAlignment","PixelMath","Resample","Rotation",
 "ScreenTransferFunction","LRGBCombination","Convolution","UnsharpMask","StarMask","RangeSelection","SplitCFA","MergeCFA","CosmeticCorrection","FastIntegration","CometAlignment","DrizzleIntegration",
 "ArcsinhStretch","ExponentialTransformation","ATrousWaveletTransform","GREYCstorationFilter","MaskedStretch","GeneralizedHyperbolicStretch","ImageSolver","ABE","DBE","BackgroundModelization","NoiseEvaluation","Statistics",
 "SampleFormatConversion","InverseFourierTransform","FourierTransform","ColorManagement","Annotation","FindingChart","Blink","LinearFit","ChannelMatch"];
var cat = {};
for (var i = 0; i < names.length; ++i) {
   var n = names[i];
   try {
      var cls = eval(n); var P = new cls; var props = {};
      for (var k in P) { try { var v = P[k]; if (typeof v != "function") props[k] = (typeof v == "object") ? JSON.stringify(v) : v; } catch (e) {} }
      var consts = {}; for (var k2 in cls.prototype) if (/^[A-Z]/.test(k2)) { try { consts[k2] = cls.prototype[k2]; } catch (e) {} }
      cat[n] = {available: true, parameters: props, constants: consts};
   } catch (e) { cat[n] = {available: false}; }
}
File.writeTextFile(OUT, JSON.stringify(cat));
