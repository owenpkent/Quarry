"""
Synthesia Block Detector (Prototype A1)
Extracts MIDI from Synthesia-style piano tutorial videos.
"""

import argparse
import cv2
import numpy as np
from dataclasses import dataclass
from typing import List, Tuple, Optional
from midiutil import MIDIFile


@dataclass
class NoteEvent:
    """Represents a detected note."""
    midi_note: int
    start_time: float
    end_time: float
    velocity: int = 100


@dataclass 
class KeyboardRegion:
    """Detected keyboard location in frame."""
    x: int
    y: int
    width: int
    height: int
    white_key_width: float
    first_note: int = 21  # A0


class SynthesiaDetector:
    """Detects falling blocks in Synthesia-style videos."""
    
    # Standard 88-key piano range
    PIANO_KEYS = 88
    FIRST_MIDI_NOTE = 21  # A0
    
    # Common Synthesia block colors (BGR format)
    BLOCK_COLORS = [
        (0, 255, 0),    # Green (right hand)
        (0, 200, 0),    # Dark green
        (255, 100, 0),  # Blue (left hand) 
        (255, 150, 50), # Light blue
        (0, 255, 255),  # Yellow
        (0, 165, 255),  # Orange
        (255, 0, 255),  # Magenta
        (255, 0, 0),    # Blue
    ]
    
    def __init__(self, threshold: int = 30, min_block_height: int = 5):
        self.threshold = threshold
        self.min_block_height = min_block_height
        self.keyboard_region: Optional[KeyboardRegion] = None
        
    def detect_keyboard(self, frame: np.ndarray) -> Optional[KeyboardRegion]:
        """
        Detect piano keyboard region in frame.
        Looks for alternating white/black key pattern at bottom of frame.
        """
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        height, width = gray.shape
        
        # Search bottom 40% of frame for keyboard
        search_start = int(height * 0.6)
        search_region = gray[search_start:, :]
        
        # Look for row with high contrast (white/black keys)
        row_contrasts = []
        for y in range(search_region.shape[0]):
            row = search_region[y, :]
            # Calculate local contrast
            contrast = np.std(row)
            row_contrasts.append(contrast)
        
        if not row_contrasts:
            return None
            
        # Find region with highest contrast
        best_y = np.argmax(row_contrasts)
        keyboard_y = search_start + best_y
        
        # Estimate keyboard bounds
        # Keyboard typically spans most of frame width
        keyboard_x = int(width * 0.05)
        keyboard_width = int(width * 0.9)
        keyboard_height = int(height * 0.15)
        
        # Estimate white key width (52 white keys in 88-key piano)
        white_key_width = keyboard_width / 52.0
        
        return KeyboardRegion(
            x=keyboard_x,
            y=keyboard_y,
            width=keyboard_width,
            height=keyboard_height,
            white_key_width=white_key_width
        )
    
    def x_to_midi_note(self, x: int, keyboard: KeyboardRegion) -> int:
        """Convert x position to MIDI note number."""
        # Relative position within keyboard
        rel_x = x - keyboard.x
        
        # Map to white key index (0-51)
        white_key_idx = int(rel_x / keyboard.white_key_width)
        white_key_idx = max(0, min(51, white_key_idx))
        
        # Convert white key index to MIDI note
        # White keys follow pattern: A(0), B(1), C(2), D(3), E(4), F(5), G(6), A(7)...
        # Starting from A0 (MIDI 21)
        
        # Map white key to semitone offset from A0
        octave = white_key_idx // 7
        key_in_octave = white_key_idx % 7
        
        # Semitone offsets for white keys starting from A
        white_key_semitones = [0, 2, 3, 5, 7, 8, 10]  # A, B, C, D, E, F, G
        
        semitone = octave * 12 + white_key_semitones[key_in_octave]
        midi_note = self.FIRST_MIDI_NOTE + semitone
        
        return max(21, min(108, midi_note))
    
    def detect_blocks(self, frame: np.ndarray, keyboard: KeyboardRegion) -> List[Tuple[int, int, int, int]]:
        """
        Detect colored blocks above keyboard.
        Returns list of (x, y, width, height) bounding boxes.
        """
        # Region above keyboard where blocks fall
        block_region = frame[:keyboard.y, :]
        
        detected_blocks = []
        
        for color in self.BLOCK_COLORS:
            # Create mask for this color
            lower = np.array([max(0, c - self.threshold) for c in color])
            upper = np.array([min(255, c + self.threshold) for c in color])
            mask = cv2.inRange(block_region, lower, upper)
            
            # Find contours
            contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
            
            for contour in contours:
                x, y, w, h = cv2.boundingRect(contour)
                
                # Filter by minimum height (actual notes, not noise)
                if h >= self.min_block_height and w >= 3:
                    detected_blocks.append((x, y, w, h))
        
        return detected_blocks
    
    def process_video(self, video_path: str, start_time: float = 0, 
                      end_time: Optional[float] = None, preview: bool = False) -> List[NoteEvent]:
        """
        Process video and extract note events.
        """
        cap = cv2.VideoCapture(video_path)
        
        if not cap.isOpened():
            raise ValueError(f"Could not open video: {video_path}")
        
        fps = cap.get(cv2.CAP_PROP_FPS)
        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        duration = total_frames / fps
        
        print(f"Video: {video_path}")
        print(f"FPS: {fps:.2f}, Duration: {duration:.2f}s, Frames: {total_frames}")
        
        if end_time is None:
            end_time = duration
            
        start_frame = int(start_time * fps)
        end_frame = int(end_time * fps)
        
        cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
        
        # Detect keyboard in first frame
        ret, first_frame = cap.read()
        if not ret:
            raise ValueError("Could not read first frame")
            
        self.keyboard_region = self.detect_keyboard(first_frame)
        if self.keyboard_region is None:
            raise ValueError("Could not detect keyboard in video")
            
        print(f"Keyboard detected at y={self.keyboard_region.y}")
        
        cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
        
        # Track active notes: midi_note -> (start_frame, last_seen_frame)
        active_notes = {}
        note_events = []
        
        frame_idx = start_frame
        
        while frame_idx < end_frame:
            ret, frame = cap.read()
            if not ret:
                break
                
            blocks = self.detect_blocks(frame, self.keyboard_region)
            
            # Track which notes are seen this frame
            seen_notes = set()
            
            for x, y, w, h in blocks:
                # Get MIDI note from center x position
                center_x = x + w // 2
                midi_note = self.x_to_midi_note(center_x, self.keyboard_region)
                seen_notes.add(midi_note)
                
                # Check if this is a new note or continuation
                if midi_note not in active_notes:
                    active_notes[midi_note] = (frame_idx, frame_idx)
                else:
                    start, _ = active_notes[midi_note]
                    active_notes[midi_note] = (start, frame_idx)
            
            # Check for notes that ended (not seen for a few frames)
            gap_threshold = 3  # frames
            ended_notes = []
            
            for midi_note, (start, last_seen) in active_notes.items():
                if midi_note not in seen_notes:
                    if frame_idx - last_seen > gap_threshold:
                        # Note ended
                        start_time_s = (start - start_frame) / fps
                        end_time_s = (last_seen - start_frame) / fps
                        
                        if end_time_s > start_time_s:
                            note_events.append(NoteEvent(
                                midi_note=midi_note,
                                start_time=start_time_s,
                                end_time=end_time_s
                            ))
                        ended_notes.append(midi_note)
            
            for note in ended_notes:
                del active_notes[note]
            
            # Preview visualization
            if preview:
                vis_frame = frame.copy()
                
                # Draw keyboard region
                cv2.rectangle(vis_frame, 
                    (self.keyboard_region.x, self.keyboard_region.y),
                    (self.keyboard_region.x + self.keyboard_region.width, 
                     self.keyboard_region.y + self.keyboard_region.height),
                    (0, 255, 255), 2)
                
                # Draw detected blocks
                for x, y, w, h in blocks:
                    cv2.rectangle(vis_frame, (x, y), (x + w, y + h), (0, 255, 0), 2)
                    center_x = x + w // 2
                    midi_note = self.x_to_midi_note(center_x, self.keyboard_region)
                    cv2.putText(vis_frame, str(midi_note), (x, y - 5),
                               cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 0), 1)
                
                # Show frame info
                current_time = (frame_idx - start_frame) / fps
                cv2.putText(vis_frame, f"Time: {current_time:.2f}s  Notes: {len(seen_notes)}", 
                           (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
                
                cv2.imshow("Synthesia Detector", vis_frame)
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
            
            frame_idx += 1
            
            # Progress
            if frame_idx % 100 == 0:
                progress = (frame_idx - start_frame) / (end_frame - start_frame) * 100
                print(f"Processing: {progress:.1f}%", end='\r')
        
        # Finalize any remaining active notes
        for midi_note, (start, last_seen) in active_notes.items():
            start_time_s = (start - start_frame) / fps
            end_time_s = (last_seen - start_frame) / fps
            if end_time_s > start_time_s:
                note_events.append(NoteEvent(
                    midi_note=midi_note,
                    start_time=start_time_s,
                    end_time=end_time_s
                ))
        
        cap.release()
        if preview:
            cv2.destroyAllWindows()
        
        print(f"\nDetected {len(note_events)} note events")
        return note_events
    
    def export_midi(self, note_events: List[NoteEvent], output_path: str, 
                    tempo: int = 120) -> None:
        """Export note events to MIDI file."""
        midi = MIDIFile(1)  # One track
        track = 0
        channel = 0
        time = 0
        
        midi.addTempo(track, time, tempo)
        midi.addProgramChange(track, channel, time, 0)  # Piano
        
        for event in note_events:
            # Convert time in seconds to beats
            beat_start = event.start_time * (tempo / 60.0)
            duration_beats = (event.end_time - event.start_time) * (tempo / 60.0)
            
            midi.addNote(track, channel, event.midi_note, 
                        beat_start, duration_beats, event.velocity)
        
        with open(output_path, 'wb') as f:
            midi.writeFile(f)
        
        print(f"Saved MIDI to: {output_path}")


def main():
    parser = argparse.ArgumentParser(description="Extract MIDI from Synthesia videos")
    parser.add_argument("video", help="Input video file path")
    parser.add_argument("-o", "--output", default="output.mid", help="Output MIDI file")
    parser.add_argument("--preview", action="store_true", help="Show detection preview")
    parser.add_argument("--threshold", type=int, default=30, help="Color detection threshold")
    parser.add_argument("--start", type=float, default=0, help="Start time in seconds")
    parser.add_argument("--end", type=float, default=None, help="End time in seconds")
    parser.add_argument("--tempo", type=int, default=120, help="MIDI tempo (BPM)")
    
    args = parser.parse_args()
    
    detector = SynthesiaDetector(threshold=args.threshold)
    
    try:
        note_events = detector.process_video(
            args.video,
            start_time=args.start,
            end_time=args.end,
            preview=args.preview
        )
        
        if note_events:
            detector.export_midi(note_events, args.output, tempo=args.tempo)
        else:
            print("No notes detected")
            
    except Exception as e:
        print(f"Error: {e}")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())
