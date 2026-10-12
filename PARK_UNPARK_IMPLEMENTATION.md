# Park and Unpark Implementation Using Slew Command

## Overview
This implementation uses the `Slew` command to control altitude, exactly as requested. It follows the same pattern as your existing GUI slew functionality and monitors altitude periodically to detect when the target position is reached.

## Implementation Strategy

Uses the proven `Slew` command pattern from your code:
1. Send `Slew` command with `AltRate` and `AzmRate` parameters
2. Monitor altitude from mount status every 500ms
3. Stop motion (send Slew with rate=0) when target reached (±2° tolerance)
4. Safety timeouts and bounds checking

## Commands Used
- **`Slew`** - Same command as GUI manual slew
  - `AltRate`: -9 (down) or 9 (up)
  - `AzmRate`: 0 (no azimuth motion)
  - Stop by setting both rates to 0

## Park/Unpark Targets

| Operation | Altitude | Command |
|-----------|----------|---------|
| Park | -10° | `Slew` with `AltRate: -9` |
| Unpark | +60° | `Slew` with `AltRate: 9` |

## WebSocket Commands

### Park (start moving down)
```json
{"Command": "Slew", "Destination": "Mount", "AltRate": -9, "AzmRate": 0}
```

### Unpark (start moving up)
```json
{"Command": "Slew", "Destination": "Mount", "AltRate": 9, "AzmRate": 0}
```

### Stop (at target)
```json
{"Command": "Slew", "Destination": "Mount", "AltRate": 0, "AzmRate": 0}
```

## Code Example

```cpp
bool OriginBackend::parkMount()
{
    // Start slewing down
    int altRate = -9;  // Same rate as GUI
    
    QJsonObject slewCommand;
    slewCommand["AltRate"] = altRate;
    slewCommand["AzmRate"] = 0;
    
    sendCommand("Slew", "Mount", slewCommand);
    m_parkMonitorTimer->start();
    
    return true;
}

void OriginBackend::stopParkMotion()
{
    // Stop by setting rates to zero
    QJsonObject slewCommand;
    slewCommand["AltRate"] = 0;
    slewCommand["AzmRate"] = 0;
    
    sendCommand("Slew", "Mount", slewCommand);
}
```

## Safety Features
- ±2° tolerance (prevents oscillation)
- 5° overshoot limit (emergency stop)
- 60s timeout (park), 90s timeout (unpark)
- Rate 9 (same as proven GUI slew)

## Usage

```cpp
originBackend->parkMount();    // Move to -10°
originBackend->unparkMount();  // Move to +60° and initialize
```

## Files Modified
- OriginBackend.hpp - Add methods, signals (~30 lines)
- OriginBackend.cpp - Implementation (~200 lines)
- TelescopeGUI.cpp - UI buttons (~50 lines)

## Advantages
1. Uses proven `Slew` command from GUI
2. Same rate (9) as manual slew operations
3. Continuous altitude monitoring (500ms checks)
4. Graceful stop (zero-rate command)
5. Consistent with existing code patterns
