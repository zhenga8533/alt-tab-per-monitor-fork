# Alt+Tab per monitor - Fork

A fork of the [Windhawk](https://windhawk.net/) mod "Alt+Tab per monitor" by L3r0y.
It makes Alt+Tab show only the windows on the monitor where the cursor is.

- `source.cpp` is the original mod, kept as a reference.
- `fork.cpp` is this fork.

## Changes from the original

- Keeps filtering windows for the whole time Alt+Tab is open, using the
  `v_DismissView` hooks. The original stopped filtering 200 ms after Alt+Tab
  opened. Windows that became active or flashed after that point, such as a
  terminal with a running CLI, showed up on every monitor.
- Uses the monitor the cursor was on when Alt+Tab opened, instead of checking
  the cursor position again on every call.
- Adds a "Current monitor" setting: the monitor under the cursor, or the
  monitor of the focused window.
- Can show all windows when the current monitor has none.
- Adds an "Always show apps" list of apps that appear in Alt+Tab on every
  monitor.

## Install

In Windhawk, create a new mod, paste the contents of `fork.cpp`, and compile it.
