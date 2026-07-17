# Optional vocal models

Model weights are not stored in this repository. Use only models and voices for which you have the necessary licence and performer consent.

When `SSBB_ENABLE_ONNX=ON`, the initial bridge looks for:

- `acoustic.onnx`
- `vocoder.onnx`

The example contract uses acoustic inputs named `tokens` and `controls`, acoustic output `acoustic_features`, and vocoder output `waveform`. Adapt the bridge to the actual model metadata before enabling it in a release.

Do not add artist-imitation, non-consensual voice-cloning, or unlicensed model assets.
