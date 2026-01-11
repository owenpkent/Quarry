# Synthesia Block Detector (Prototype A1)

Extracts MIDI from Synthesia-style piano tutorial videos by detecting falling blocks.

## Setup

```powershell
cd prototypes\synthesia_detector
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

## Usage

```powershell
python synthesia_detector.py input_video.mp4 -o output.mid
```

### Options

- `-o, --output` — Output MIDI file (default: output.mid)
- `--preview` — Show detection preview window
- `--threshold` — Detection sensitivity 0-255 (default: 30)
- `--start` — Start time in seconds
- `--end` — End time in seconds

## How It Works

1. **Keyboard Detection** — Locates piano keyboard region in frame
2. **Block Tracking** — Detects colored rectangles above keyboard
3. **Note Mapping** — Maps horizontal position to MIDI note (21-108)
4. **Timing** — Converts frame position to time using video FPS
5. **MIDI Export** — Generates standard MIDI file

## Limitations

- Works best with standard Synthesia color schemes
- Assumes keyboard is at bottom of frame
- May need threshold tuning for different video qualities
