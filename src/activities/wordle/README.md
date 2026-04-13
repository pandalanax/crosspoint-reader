# Wordle Activity

A classic Wordle word-guessing game for the Xteink X4 e-reader.

Based on [NathanMoore4472/CrossWordle](https://github.com/NathanMoore4472/CrossWordle).

## Setup

Word lists are needed on the SD card. Two options:

### Option A: Auto-download (WiFi required)
1. Open Wordle from the home menu
2. Connect to WiFi first
3. Press OK to download word lists automatically

### Option B: Manual
1. Create `/wordle/` folder on SD card
2. Download these files and place them there:
   - `wordles.json` — answer words (~2300 words)
   - `nonwordles.json` — valid guesses that aren't answers (~10000 words)
3. Source: [stuartpb/wordles](https://github.com/stuartpb/wordles)

## Controls

| Button | Action |
|--------|--------|
| Up/Down/Left/Right | Navigate on-screen keyboard |
| Confirm | Type selected letter / Submit guess (when 5 letters entered) |
| Back | Exit to home |
| Up (from top row) | Show legend/guide |
| Any key (game over) | Start new game |

## How It Works

- Guess a 5-letter word in 6 tries
- After each guess, tiles show feedback:
  - **Black tile + underline**: Correct letter, correct position
  - **Black tile + dot**: Correct letter, wrong position
  - **Black tile (plain)**: Letter not in the word
  - **White tile**: Not yet guessed
- Keyboard keys also show their best known state
- Invalid words show a "Not in word list" popup
- Auto-sleep is disabled during gameplay

## Files

| Path | Description |
|------|-------------|
| `/wordle/wordles.json` | Answer word list (SD card) |
| `/wordle/nonwordles.json` | Valid guess word list (SD card) |
