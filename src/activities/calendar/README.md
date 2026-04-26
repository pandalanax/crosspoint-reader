# Calendar

Month grid calendar that syncs with a CalDAV server (tested with [Radicale](https://radicale.org/)). Browse your schedule offline and create quick "Meeting" events on the go.

## Setup

1. Copy [`templates/caldav.template.json`](../../../templates/caldav.template.json) to `/.crosspoint/caldav.json` on the SD card
2. Edit the file with:
   - **calendarUrl** -- the CalDAV calendar collection URL, ending with `/` (e.g. `https://cal.example.com/user/calendar-id/`)
   - **username**
   - **password**
3. Safely eject the SD card and start the Calendar activity

The server must support CalDAV `REPORT` requests with calendar queries. Radicale does.
Do not use a subscribed `.ics` export URL here; use the writable calendar collection URL instead.

## How It Works

- On first open, connects to WiFi and fetches only the needed calendar window (30 days back, 120 days forward)
- Uses CalDAV `calendar-query` with server-side expansion, so recurring events in that window are included
- Parses normal timed events, all-day events, and multi-day events
- Treats UTC timestamps as Europe/Brussels local time on-device
- Caches events to SD card for offline browsing
- On subsequent opens, shows cached events instantly
- Refresh status, errors, and success are shown as popups over the calendar view

## Controls

### Month View

| Button | Action |
|--------|--------|
| Up / Down / Left / Right | Move day cursor |
| PageForward | Next month |
| PageBack | Previous month |
| Confirm (short press) | View events on selected day |
| Confirm (long press, 1s) | Add a new event on selected day |
| Back (short press) | Go home |
| Back (long press, 1s) | Refresh from server (reconnects WiFi) |

### Day Detail View

| Button | Action |
|--------|--------|
| Up / Down | Scroll through events |
| Back | Return to month view |

### Time Picker (adding events)

| Button | Action |
|--------|--------|
| Up / Down | Change hour or minute (5-min steps for minutes) |
| Left / Right | Switch between hour and minute |
| Confirm | Save event |
| Back | Cancel, return to month view |

## On-the-Go Event Creation

Long-press Confirm on any day to open a time picker. Select hour and minute, press Confirm to save a "Meeting" event. The event is:

1. Added to the local display immediately
2. Saved to `/.crosspoint/calendar_pending.json` as a pending event
3. Uploaded to your CalDAV server on the next WiFi refresh (long-press Back)

Failed uploads are retried on subsequent refreshes. You can rename the "Meeting" event later from another device.

## Files

| Path | Purpose |
|------|---------|
| `/.crosspoint/caldav.json` | CalDAV URL, username, and password |
| `/.crosspoint/calendar_cache.json` | Cached events (auto-created on fetch) |
| `/.crosspoint/calendar_pending.json` | Events created offline, awaiting sync |

## Display

- **Today**: filled black circle with white text
- **Cursor**: thick border rectangle
- **Event indicator**: small dot below the day number
- **Refresh / errors**: popup overlay, calendar stays visible in the background
