# Park/Unpark Quick Reference - Slew Command

## Quick Commands

### Park
```cpp
originBackend->parkMount();  // Move to Alt: -10°
```

### Unpark
```cpp
originBackend->unparkMount();  // Move to Alt: +60°, initialize
```

---

## How It Works

1. Send `Slew` command with rate -9 (park) or +9 (unpark)
2. Monitor altitude every 500ms
3. Stop when within 2° of target (send Slew with rate 0)

---

## WebSocket Commands

**Start Park:**
```json
{"Command": "Slew", "Destination": "Mount", "AltRate": -9, "AzmRate": 0}
```

**Start Unpark:**
```json
{"Command": "Slew", "Destination": "Mount", "AltRate": 9, "AzmRate": 0}
```

**Stop:**
```json
{"Command": "Slew", "Destination": "Mount", "AltRate": 0, "AzmRate": 0}
```

---

## Signals

```cpp
void parkStarted()
void parkCompleted()
void unparkStarted()
void unparkCompleted()
void trackingError(QString)
```

---

## Target Altitudes

| Operation | Target | Tolerance |
|-----------|--------|-----------|
| Park | -10° | ±2° |
| Unpark | +60° | ±2° |

---

## Timing

| Operation | Time | Timeout |
|-----------|------|---------|
| Park | 15-35s | 60s |
| Unpark | 25-50s | 90s |

---

## Rate Parameter

- **-9**: Move down (parking)
- **9**: Move up (unparking)
- **0**: Stop motion
- Same as GUI manual slew rate

---

## Signal Example

```cpp
connect(originBackend, &OriginBackend::parkCompleted,
        this, []() { qDebug() << "Parked!"; });
```

---

## Debug Output

```
Parking telescope
Current altitude: 45.2
Target altitude: -10°
Park monitoring - Target: -10 Current: 22.5
Park monitoring - Target: -10 Current: -9.1
Park position reached!
Stopping altitude motion
```

---

## Comparison to GUI Slew

Same `Slew` command, same rate (9), but:
- **GUI**: User controls start/stop
- **Park**: Automatic stop at target altitude
