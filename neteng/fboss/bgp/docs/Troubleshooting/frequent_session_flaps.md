# BGP highly frequent session flaps

[original doc with logs](https://docs.google.com/document/d/1REg_0MTEqWif20Z5xG-df992jQKfDeb8Tp28L-YIyZc/edit?usp=sharing)

This issue happens when BGP had frequent session flaps.

## Checklist

1. Investigate why BGP has frequent session flaps. Run

   ```
   fbagentc --host [switch-name] --port 6909 getCounters | grep sessionStateChanges
   ```

   to see which peer(s) are flapping

2. Run

   ```
   fbagentc --host [switch-name] --port 6909 getCounters | grep peerError
   ```

   to see why they are flapping.

3. Run

   ```
   fbagentc --host [switch-name] --port 6909 getCounters | grep ucmp
   ```

   to see UCMP configs

## issues

Possible causes: (TBD)
