"""Audio loading for kong, in place of piano_transcription_inference.load_audio.

The library's own loader (utilities.py, "copied from librosa.core.load() except that ffmpeg
backend is always used") calls `librosa.core.audio.resample(y, sr_native, sr, res_type=...)`
with positional sample rates. librosa made those keyword-only in 0.10.0, so that call raises a
TypeError on every librosa new enough to have stopped importing pkg_resources (0.10.2 and up).
piano_transcription_inference 0.0.6 is the latest release and still has the positional call.

This is the same loader with the same steps in the same order, so kong hears the same samples:
decode through ffmpeg to 16-bit PCM, scale to float32, average the channels, resample with
resampy's kaiser_best filter. Only the offset/duration cropping, which nothing here used, is
left out. Shared by the sidecar (engines.py) and the bake-off (run_bakeoff.py).

Go back to the library's load_audio, and delete this file, once a release of it passes the
sample rates by keyword.
"""

import os

import audioread
import audioread.ffdec
import librosa
import numpy as np


def load_audio(path, sr, mono=True, dtype=np.float32, res_type="kaiser_best"):
    """Returns (samples, sr), as piano_transcription_inference.load_audio(path, sr=sr, mono=mono)
    does."""
    frames = []

    with audioread.audio_open(os.path.realpath(path), backends=[audioread.ffdec.FFmpegAudioFile]) as input_file:
        sr_native = input_file.samplerate
        n_channels = input_file.channels

        for frame in input_file:
            frames.append(librosa.util.buf_to_float(frame, n_bytes=2, dtype=dtype))

    y = frames

    if frames:
        y = np.concatenate(frames)

        if n_channels > 1:
            y = y.reshape((-1, n_channels)).T
            if mono:
                y = librosa.to_mono(y)

        if sr is not None:
            y = librosa.resample(y, orig_sr=sr_native, target_sr=sr, res_type=res_type)
        else:
            sr = sr_native

    return np.ascontiguousarray(y, dtype=dtype), sr
