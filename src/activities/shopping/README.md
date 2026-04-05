# Shopping List

Fetches your shopping list from a self-hosted [Tandoor Recipes](https://tandoor.dev/) instance, caches it on the SD card, and lets you check off items while offline.

## Setup

1. Open the device's web interface (connect via WiFi, go to `http://crosspoint.local`)
2. In Settings, fill in:
   - **Tandoor Server URL** -- your instance URL (e.g. `https://tandoor.example.com`)
   - **Tandoor API Token** -- generate one in Tandoor under User > API Tokens

## How It Works

- On first open, connects to WiFi and fetches the current shopping list
- Saves to SD card as `/.crosspoint/shopping_list.json` for offline use
- On subsequent opens, shows the cached list instantly (no WiFi needed)
- Items are grouped by food category with section headers

## Controls

### List View

| Button | Action |
|--------|--------|
| Up / Down | Scroll through items |
| Confirm | Toggle checked/unchecked |
| Back (short press) | Go home |
| Back (long press, 1s) | Refresh from Tandoor (reconnects WiFi) |

## Files

| Path | Purpose |
|------|---------|
| `/.crosspoint/shopping_list.json` | Cached shopping list (auto-created) |

## Sleep Behavior

- After a WiFi refresh completes, auto-sleep is allowed (device can sleep on timeout)
- Any button press (scrolling, checking items) keeps the device awake
