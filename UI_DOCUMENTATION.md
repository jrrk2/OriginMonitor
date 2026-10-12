# Park/Unpark UI Implementation

## Location
The park/unpark controls are added to the **Mount** tab at the bottom.

## Visual Layout

```
┌─────────────────────────────────────────────────────┐
│ Mount Tab                                           │
├─────────────────────────────────────────────────────┤
│                                                     │
│ Battery Current:        2.5 A                       │
│ Battery Level:          85%                         │
│ Battery Voltage:        12.6 V                      │
│ Charger Status:         Charging                    │
│ Time:                   14:30:45                    │
│ Date:                   2024-12-18                  │
│ Time Zone:              UTC                         │
│ Latitude:               52.2053°                    │
│ Longitude:              0.1218°                     │
│ Is Aligned:             Yes                         │
│ Is Tracking:            Yes                         │
│ Is Goto Over:           Yes                         │
│ Num Align Refs:         3                           │
│ Azimuth:                180.5°                      │
│ Azimuth Error:          0.2°                        │
│ Altitude:               45.3°                       │
│ Altitude Error:         0.1°                        │
│ Last Update:            2024-12-18 14:30:45         │
│                                                     │
│ ─────────────────────────────────────────────────  │ ← Separator line
│                                                     │
│ ┌─ Park/Unpark Controls ──────────────────────┐   │
│ │                                              │   │
│ │ Park: Move to Alt -10° (safe storage        │   │
│ │ position)                                    │   │
│ │ Unpark: Move to Alt +60° and initialize     │   │
│ │                                              │   │
│ │  ┌──────────────────┐  ┌─────────────────┐ │   │
│ │  │ Park Telescope   │  │ Unpark Telescope│ │   │
│ │  └──────────────────┘  └─────────────────┘ │   │
│ │                                              │   │
│ └──────────────────────────────────────────────┘   │
│                                                     │
└─────────────────────────────────────────────────────┘
```

## Button Behavior

### Park Telescope Button
- **Tooltip**: "Move telescope to parked position (Alt: -10°)"
- **On Click**:
  - Checks if telescope is connected
  - If not connected: Shows warning dialog
  - If connected: Calls `originBackend->parkMount()`
  - Telescope moves to -10° altitude
  - Status changes to "Parking" then "Parked"

### Unpark Telescope Button
- **Tooltip**: "Move telescope to operational position (Alt: 60°) and initialize"
- **On Click**:
  - Checks if telescope is connected
  - If not connected: Shows warning dialog
  - If connected: Calls `originBackend->unparkMount()`
  - Telescope moves to +60° altitude
  - Automatically initializes telescope
  - Status changes to "Unparking" → "Initializing" → "Idle"

## User Workflow

### End of Observing Session
1. User clicks **"Park Telescope"** button
2. Telescope slews to Alt: -10° (pointing down)
3. Button shows operation in progress
4. After ~15-35 seconds, telescope parked
5. Mount status shows "Parked"

### Start of Observing Session
1. User clicks **"Unpark Telescope"** button
2. Telescope slews to Alt: +60° (pointing up)
3. Telescope automatically initializes
4. After ~25-50 seconds, telescope ready
5. Mount status shows "Idle"

## Visual States

### During Park Operation
```
Status: Parking
Altitude: 35.2° → 22.5° → 8.9° → -9.1°
Console: "Park monitoring - Target: -10 Current: 8.9"
```

### During Unpark Operation
```
Status: Unparking
Altitude: -9.8° → 15.3° → 35.7° → 59.2°
Console: "Park monitoring - Target: 60 Current: 59.2"
Then: Status: Initializing
Finally: Status: Idle
```

## Error Handling

### Not Connected Error
```
┌──────────────────────────────────┐
│         Not Connected            │
├──────────────────────────────────┤
│ Please connect to telescope      │
│ first                            │
│                                  │
│            [ OK ]                │
└──────────────────────────────────┘
```

### Timeout Error
```
Console: "Park timeout - stopping motion"
Dialog: "Park operation timed out"
```

### Overshoot Error
```
Console: "Park altitude exceeded safe limit, stopping"
Dialog: "Park altitude exceeded safe limit"
```

## Code Structure

The buttons use Qt's signal/slot mechanism:

```cpp
connect(parkButton, &QPushButton::clicked, this, [this]() {
    if (originBackend->isConnected()) {
        qDebug() << "Park button clicked";
        originBackend->parkMount();
    } else {
        QMessageBox::warning(this, "Not Connected", 
                           "Please connect to telescope first");
    }
});
```

## Integration with Existing UI

The park/unpark controls:
- ✓ Match existing UI style
- ✓ Use same layout patterns (QGroupBox, QGridLayout)
- ✓ Consistent button sizing
- ✓ Follow Qt naming conventions
- ✓ Use lambda connections like other buttons
- ✓ Include tooltips for user guidance
- ✓ Check connection state before operation

## Styling

The controls automatically inherit your application's style:
- Font matches other labels
- Button size matches other buttons
- Colors match application theme
- Info label uses gray text (10px font)
- GroupBox has standard border

## Files Modified

To add these buttons, only 3 lines were changed in the TelescopeGUI files:

**TelescopeGUI.hpp:**
- Added `#include <QMessageBox>`
- Added `#include <QFrame>`

**TelescopeGUI.cpp:**
- Modified `createMountTab()` to add park controls (~50 lines)

All other tabs and functionality remain unchanged.
