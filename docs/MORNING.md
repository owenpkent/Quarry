# Quarry — morning brief

## What Quarry is now

Quarry listens to whatever your computer is playing, all the time, and keeps the last five
minutes in memory. When you hear something good you click one button — **KEEP** — and the
last thirty seconds becomes MIDI you can drag straight onto an Ableton track. It also tells
you which notes it was unsure about, so you know which bars to go fix, and it fixes them with
the same mouse-only piano roll from Lattice.

Nothing gets written to your drive until you press KEEP. Nothing gets sent anywhere.

## The one big decision

**KEEP replaces a Record button.**

Every capture tool ever made has the same problem: you weren't recording when the good thing
happened. A Record button means you have to press it *before* you know you want it, then press
Stop, then press Transcribe — three clicks and two moments of timing. KEEP is one click and no
timing at all, because Quarry was already listening. And if you grab the wrong thirty seconds,
you drag the edge of the highlighted region and it re-does it in half a second — so the first
grab never has to be right.

The cost: Quarry uses about 115 MB of memory sitting there listening. That's it.

I also **cut the scrolling live readout** that showed tempo and key while you listen. Its
numbers would have been wrong often enough to be noise, and it was eating the screen space the
piano roll needs.

## Built overnight

**You can record what your computer is playing.** Double-click `run.py`. The audio input
panel has a new **System Audio** driver at the top, already selected, pointed at your default
output. Play a YouTube video, hit the big RECORD button, hit stop — transcription starts on
its own (it already did that; it didn't need building) — then drag the MIDI onto an Ableton
track. That is the whole thesis working end to end.

It builds clean. **I have not run it**, because running it opens a window and grabs audio
while you're asleep. So treat the first launch as the real test.

Also landed: a `CHANGELOG.md` and a `NOTICE` file (Apache-2.0 §4(b) requires a fork to credit
the original and state its changes; neither had ever existed here), and the design docs in
`docs/`.

**Not done, deliberately:** the rename, and committing. Both are waiting on you — see below.

## Two things I did not do

**I didn't commit.** The 414-line change is sitting in your working tree on
`audio-input-and-recording`. Look at it, then commit it yourself, or tell me to.

**I didn't rename.** The full checklist is ready and I verified the dangerous parts of it, but
renaming now would bury the change above under a ~200-file diff and make it unreviewable. Say
the word and it's one pass. Three things you should know before you do:

- It **breaks saved sessions** — the plugin's unique ID is derived from the plugin code and
  bundle ID. Nothing has shipped, so now is the free moment to pay it.
- The GitHub repo rename needs **your account**; I can't do that part.
- Renaming the `NeuralNote/` source directory makes every future merge from DamRsn's upstream
  a manual conflict. Worth it, I think, but it's a real cost and it's your call.

I also found a live bug while checking: the update checker points at
`github.com/DamRsn/NeuralNote/releases`, so anyone who checks for updates is offered
*upstream's* installers, not yours. Trivial fix, folded into the rename.

## Five questions

**1. How much "describing" do you actually want?**
Confidence, tempo and key are cheap and clearly useful. Chords, section names and a
written-paragraph summary are about two extra weeks and they're often wrong.
**A)** Cut chords, sections and the paragraph — spend the time on getting the notes right.
**B) ← recommended.** Keep chords (only if I can measure them getting it right on 20 of your
own captures), keep section *boundaries* without naming them, keep a simple written summary.
**C)** Build all four properly.

**2. Should Quarry ever talk to the internet?**
The written summary is much better if it goes through Claude. No audio would ever leave the
machine — just a small list of facts like "F# minor, 92 BPM, four sections".
**A)** Never. Offline only.  **B) ← recommended for now.** Offline only to start; add it later
behind an off-by-default switch, with your own key file.  **C)** Build it now.

**3. ASIO audio drivers — keep or drop?**
Steinberg needs a signed agreement before you can sell anything containing their ASIO code.
You don't need ASIO for either source you picked.
**A) ← recommended.** Drop it. Windows' own drivers cover everything.  **B)** Keep it, sign the
agreement.

**4. Are you going to sell Quarry?**
A closed-source product needs a paid JUCE licence. This changes the build settings on day one
and is annoying to reverse later.
**A) ← recommended.** Yes — buy the JUCE licence now.  **B)** Yes, but ship with the JUCE
splash screen first.  **C)** Keep it open-source and free.

**5. Record-and-stop, or always-listening? — this is the big one.**
The design above commits to KEEP: Quarry listens constantly and one click keeps the last
thirty seconds. But what actually got built last night is the ordinary thing — arm, Record,
Stop. So by the time you read this you'll have *used* one of the two, which makes you the only
person who can settle it. Getting this wrong is a full rewrite of the shell, not a tweak.
**A)** Keep Record/Stop. Simpler, no always-on thread, and the piano roll gets about nine more
rows of screen.
**B)** Full KEEP as described — always listening, retroactive, ~115 MB sitting in memory, and
the subtlest concurrency in the product.
**C) ← recommended.** The middle: no live scrolling readout and no permanent header, but the
Record button becomes an always-listening KEEP plate over a 30-second buffer. Most of the
benefit, little of the cost.

*(Also, whenever you get a minute: 20 links to Synthesia videos you'd actually want to convert.
I can't scope that feature without seeing what they look like.)*
