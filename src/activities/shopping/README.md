# Shopping List

Fetches your shopping list from a self-hosted [Tandoor Recipes](https://tandoor.dev/) instance, caches it on the SD card, and lets you check off items while offline.

## Setup

1. Copy [`templates/tandoor.template.json`](../../../templates/tandoor.template.json) to `/.crosspoint/tandoor.json` on the SD card
2. Edit the file with:
   - **serverUrl** -- your instance URL (e.g. `https://tandoor.example.com`)
   - **apiToken** -- generate one in Tandoor under `User -> API Tokens`
3. Safely eject the SD card and start the Shopping List activity

## How It Works

- On first open, connects to WiFi and fetches the current shopping list
- Saves to SD card as `/.crosspoint/shopping_list.json` for offline use
- On subsequent opens, shows the cached list instantly (no WiFi needed)
- Items are grouped by food category with section headers
- Locally checked items stay visible with strikethrough until the next successful refresh
- After a successful refresh, synced checked items disappear from the device list

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
| `/.crosspoint/tandoor.json` | Tandoor server URL and API token |
| `/.crosspoint/shopping_list.json` | Cached shopping list and pending local checkmarks |

## Sync Behavior

- Checking an item on the device only changes the local cache at first
- A checked item remains visible with strikethrough so you can still uncheck it while shopping
- Long-press `Back` to refresh and sync local checkmarks to Tandoor
- If sync succeeds, the checked item is removed from the device list
- If sync fails, the checked item stays visible with strikethrough and is retried on the next refresh
- If the item was already checked or deleted on the server, it is treated as resolved and removed from the device list on refresh
- Items that are already checked on the server are removed from the device list on refresh, even if Tandoor still returns them in the API response

## Sleep Behavior

- After a WiFi refresh completes, auto-sleep is allowed (device can sleep on timeout)
- Any button press (scrolling, checking items) keeps the device awake
