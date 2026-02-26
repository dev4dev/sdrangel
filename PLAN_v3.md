# Plan v3: PAL/NTSC Colour Support for ATVMod Plugin — Final

## Implementation Status

All 11 sections implemented + 5 post-testing bugfixes applied and verified on hardware.

| Section | Description | Status | Files Modified |
|---------|-------------|--------|----------------|
| 1 | Settings: new colour fields | DONE | `atvmodsettings.h`, `atvmodsettings.cpp` |
| 2 | Colour subcarrier LUT allocation | DONE | `atvmodsource.h`, `atvmodsource.cpp` |
| 3 | Frame storage: BGR instead of grayscale | DONE | `atvmodsource.cpp` (openImage, pullVideo) |
| 4 | Inline RGB->YUV helper | DONE | `atvmodsource.h` |
| 5 | Hot path: colour pixel rendering | DONE | `atvmodsource.h` (pullImageSample) |
| 6 | Burst injection in back porch | DONE | `atvmodsource.h` (pullImageSample) |
| 7 | Per-line colour state update | DONE | `atvmodsource.cpp` (pullVideo) |
| 8 | applySettings wiring | DONE | `atvmodsource.cpp` |
| 9 | Colour bars test pattern | DONE | `atvmodsettings.h`, `atvmodsource.h`, `atvmodsource.cpp` |
| 10 | GUI controls | DONE | `atvmodgui.ui`, `atvmodgui.h`, `atvmodgui.cpp` |
| 11 | WebAPI + Swagger | DONE | `ATVMod.yaml`, `SWGATVModSettings.h/.cpp`, `atvmod.cpp` |

### Post-Testing Bugfixes

| Fix | Problem | Root Cause | Solution | File |
|-----|---------|------------|----------|------|
| B1 | Burst level too high (double correct) | Used peak-to-peak ratio directly as amplitude | Divide burst level by 2: `3/7 -> 3/14` (PAL), `4/10 -> 4/20` (NTSC) | `atvmodsource.h` |
| B2 | Burst phase 180° wrong — wrong colours | burstI/burstQ values swapped vs compositing formula | Corrected I/Q assignment (see Section 6 corrections) | `atvmodsource.h` |
| B3 | Phase drift in colour LUT at high indices | `float` precision (7 digits) causes ~28° error at index 3.2M | Changed LUT computation to `double` precision | `atvmodsource.cpp` |
| B4 | FM excursion range too narrow for colour | Max 0.5 gave ±2.5 MHz deviation; colour burst below decoder threshold | Extended FM excursion dial max from 500 to 2000 (0.05 to 2.0) | `atvmodgui.ui` |
| B5 | Colour artifacts at top of frame | VBI black lines had no colour burst; decoder couldn't lock PLL | Added burst to `pullBlackLineSample()` back porch | `atvmodsource.h` |

### Hardware Test Setup

- **Transmitter:** HackRF One Pro via SDRangel ATVMod
- **Receiver:** FPV VRX (5.8 GHz analog video receiver) with composite output to monitor
- **Standard tested:** PAL 625-line, FM modulation
- **Optimal FM excursion:** 50 (= 0.05 in settings, i.e. ±250 kHz deviation)
- **Chroma level:** 100 (= 1.0, full)

---

## Context

The SDRangel ATV modulator (`plugins/channeltx/modatv/`) previously transmitted black-and-white only —
all colour source material was force-converted to grayscale via `cv::COLOR_RGB2GRAY` before rendering.
This plan adds proper analogue colour TV encoding (PAL and NTSC) so that colour video files,
still images, camera feeds, and colour bars are transmitted with a colour subcarrier that real receivers
can decode.

The reference implementation is `_hacktv/src/video.c` (GPLv3, Philip Heron) — a battle-tested C
implementation of PAL/NTSC/SECAM encoding for HackRF. The architectural techniques are borrowed
from there; the DSP is adapted to SDRangel's float sample pipeline and C++ plugin structure.

---

## Key Parameters

All sourced from hacktv `vid_config_pal_i` / `vid_config_ntsc_m` and verified against hardware:

| Parameter          | PAL value              | NTSC value              |
|--------------------|------------------------|-------------------------|
| Colour carrier     | 4 433 618.75 Hz        | 3 579 545.45 Hz         |
| Carrier rational   | `17734475/4`           | `39375000/11`           |
| Burst position     | 5.6 us after sync edge | 5.3 us                  |
| Burst width        | 2.25 us                | 2.50 us                 |
| Burst level (p-p)  | 3/7 of white-blank     | 4/10 of white-blank     |
| Burst amplitude    | **3/14** of span       | **4/20** of span        |
| PAL burst phase    | +/-135 deg (alternating) | —                     |
| NTSC burst phase   | 180 deg (fixed)        | 180 deg                 |
| V coefficient (Ev) | 0.877                  | 0.877                   |
| U coefficient (Eu) | 0.493                  | 0.493                   |
| LUT frame period   | 4 frames               | 2 frames                |
| Burst rise time    | 0.3 us (raised-cosine) | 0.3 us (raised-cosine)  |

**Important:** Burst level in specs is peak-to-peak. The amplitude used in the code must be **half** the p-p value (3/14 for PAL, 4/20 for NTSC).

---

## Files Modified

| File | Changes |
|------|---------|
| `plugins/channeltx/modatv/atvmodsettings.h` | `ATVColourStd` enum, `ATVModInputColorBars`, 3 new settings fields |
| `plugins/channeltx/modatv/atvmodsettings.cpp` | Defaults, serialize/deserialize (indices 29-31), applySettings, debugString |
| `plugins/channeltx/modatv/atvmodsource.h` | `bgrToYUV()`, `ColourBar`, LUT members, burst in `pullImageSample()` + `pullBlackLineSample()`, colour bars case |
| `plugins/channeltx/modatv/atvmodsource.cpp` | Colour bar data, LUT generation (double precision), VBI burst in black lines, BGR frame storage, LUT offset advance |
| `plugins/channeltx/modatv/atvmodgui.h` | 3 slot declarations |
| `plugins/channeltx/modatv/atvmodgui.cpp` | Slot implementations, displaySettings, makeUIConnections |
| `plugins/channeltx/modatv/atvmodgui.ui` | Colour checkbox + PAL/NTSC combo + chroma slider + Color Bars input + FM excursion range increase |
| `plugins/channeltx/modatv/atvmod.cpp` | WebAPI get/put for 3 colour fields |
| `swagger/sdrangel/api/swagger/include/ATVMod.yaml` | 3 new properties |
| `swagger/sdrangel/code/qt5/client/SWGATVModSettings.h` | Generated: 3 new getters/setters |
| `swagger/sdrangel/code/qt5/client/SWGATVModSettings.cpp` | Generated: 3 new field implementations |

---

## Section 1 — Settings: new colour fields

**File:** `atvmodsettings.h`

Added to `ATVModSettings`:

```cpp
typedef enum {
    ATVColourPAL,   //!< PAL subcarrier 4433618.75 Hz, +/-135 deg burst, V alternates per line
    ATVColourNTSC   //!< NTSC subcarrier 3579545.45 Hz, 180 deg burst fixed
} ATVColourStd;

bool          m_colourEnabled;          //!< Enable PAL/NTSC colour subcarrier encoding
ATVColourStd  m_colourStd;              //!< PAL or NTSC colour standard
float         m_colourSubcarrierLevel;  //!< Chroma subcarrier amplitude 0.0-1.0
```

Added `ATVModInputColorBars` to the `ATVModInput` enum.

**File:** `atvmodsettings.cpp`

- `resetToDefaults()`: `m_colourEnabled = false; m_colourStd = ATVColourPAL; m_colourSubcarrierLevel = 1.0f;`
- `serialize()`: indices 29 (bool), 30 (int), 31 (int, level*100)
- `deserialize()`: reads with same indices, falls back to defaults
- `applySettings()` + `getDebugString()`: handle all three keys

---

## Section 2 — Colour subcarrier LUT: allocation and population

**File:** `atvmodsource.h` — new private members:

```cpp
std::vector<Complex> m_colourLUT;       // pre-computed subcarrier waveform {cos, sin}
uint32_t  m_colourLUTWidth;             // length of LUT in samples
uint32_t  m_colourLUTOffset;            // current position (advances per line for phase continuity)
int       m_frameCount;                 // frame counter for LUT cycling
int       m_burstLeftPoints;            // start of burst window (samples from line start)
int       m_burstWidthPoints;           // burst window duration (samples)
std::vector<float> m_burstWindow;       // raised-cosine burst envelope
```

**File:** `atvmodsource.cpp`, inside `applyStandard()`:

```cpp
if (settings.m_colourEnabled)
{
    // CRITICAL: Use double precision for LUT computation.
    // At index 3.2M, float (7 significant digits) accumulates ~28 degrees of phase error.
    double fsc = (settings.m_colourStd == ATVModSettings::ATVColourNTSC)
        ? 3579545.4545   // 39375000/11
        : 4433618.75;    // 17734475/4

    int framesInPeriod = (settings.m_colourStd == ATVModSettings::ATVColourNTSC) ? 2 : 4;
    m_colourLUTWidth = (uint32_t)(framesInPeriod) * (uint32_t)m_nbLines * m_pointsPerLine;

    m_colourLUT.resize(m_colourLUTWidth + m_pointsPerLine); // +1 line guard
    double phaseInc = 2.0 * M_PI * fsc / (double)m_tvSampleRate;
    for (uint32_t i = 0; i < m_colourLUT.size(); i++) {
        double phase = phaseInc * (double)i;
        m_colourLUT[i] = Complex(std::cos(phase), std::sin(phase));
    }

    m_colourLUTOffset = 0;
    m_frameCount      = 0;

    // Burst geometry (from hacktv vid_config_pal_i / vid_config_ntsc_m)
    float burstLeftUs  = (settings.m_colourStd == ATVModSettings::ATVColourNTSC) ? 5.3e-6f  : 5.6e-6f;
    float burstWidthUs = (settings.m_colourStd == ATVModSettings::ATVColourNTSC) ? 2.5e-6f  : 2.25e-6f;
    float lineUs = 1.0f / ((float)settings.m_nbLines * (float)settings.m_fps);
    m_burstLeftPoints  = (int)(burstLeftUs  / lineUs * m_pointsPerLine);
    m_burstWidthPoints = (int)(burstWidthUs / lineUs * m_pointsPerLine);

    // Clamp burst to back porch boundaries
    m_burstLeftPoints = std::max(m_pointsPerSync, m_burstLeftPoints);
    int bpEnd = m_pointsPerSync + m_pointsPerBP;
    if (m_burstLeftPoints + m_burstWidthPoints > bpEnd) {
        m_burstWidthPoints = std::max(0, bpEnd - m_burstLeftPoints);
    }

    // Raised-cosine burst envelope (0.3 us rise/fall)
    float burstRiseUs = 0.3e-6f;
    int risePoints = std::max(1, (int)(burstRiseUs / lineUs * m_pointsPerLine));
    m_burstWindow.resize(m_burstWidthPoints);
    for (int i = 0; i < m_burstWidthPoints; i++) {
        float env = 1.0f;
        if (i < risePoints)
            env = 0.5f * (1.0f - std::cos((float)M_PI * i / risePoints));
        else if (i > m_burstWidthPoints - 1 - risePoints)
            env = 0.5f * (1.0f - std::cos((float)M_PI * (m_burstWidthPoints - 1 - i) / risePoints));
        m_burstWindow[i] = env;
    }
}
else
{
    m_colourLUT.clear();
    m_colourLUTWidth = 0;
    m_burstWindow.clear();
}
```

---

## Section 3 — Frame storage: load BGR instead of grayscale

**File:** `atvmodsource.cpp`

`openImage()`:
```cpp
m_imageFromFile = cv::imread(qPrintable(fileName), cv::ImreadModes::IMREAD_COLOR); // BGR
```

`pullVideo()` — video file path:
```cpp
m_videoframeOriginal = colorFrame; // keep BGR; pixel loop handles both modes
```

`pullVideo()` — camera path:
```cpp
camera.m_videoframeOriginal = colorFrame; // keep BGR
```

No changes needed for `resizeImage()`, `resizeVideo()`, `resizeCamera()`, or `mixImageAndText()`.

---

## Section 4 — Inline RGB->YUV helper

**File:** `atvmodsource.h`, public section:

```cpp
// ITU-R BT.601 RGB->YUV conversion (coefficients from hacktv)
// eu_co=0.493 (U = Eu * (B-Y)), ev_co=0.877 (V = Ev * (R-Y))
static inline void bgrToYUV(const cv::Vec3b& bgr, float& Y, float& U, float& V)
{
    float r = bgr[2] / 255.0f;
    float g = bgr[1] / 255.0f;
    float b = bgr[0] / 255.0f;
    Y = 0.299f * r + 0.587f * g + 0.114f * b;
    U = 0.493f * (b - Y);
    V = 0.877f * (r - Y);
}
```

---

## Section 5 — Hot path: colour pixel rendering

**File:** `atvmodsource.h`, `pullImageSample()` — active image area

For each input type (Image, Video, Camera), the colour branch works as:

```cpp
if (m_settings.m_colourEnabled && m_colourLUTWidth > 0) {
    float Y, U, V;
    bgrToYUV(m_image.at<cv::Vec3b>(m_imageLine, pointIndex), Y, U, V);

    // PAL: V alternates sign on alternate lines
    float Vmod = (m_settings.m_colourStd == ATVModSettings::ATVColourPAL
                  && (m_lineCount & 1)) ? -V : V;

    uint32_t lutIdx = (m_colourLUTOffset + (uint32_t)m_horizontalCount) % m_colourLUTWidth;
    const Complex& sc = m_colourLUT[lutIdx];

    // Composite: Y + (U*sin(wt) + V*cos(wt)) * chromaLevel
    float chroma = (U * sc.imag() + Vmod * sc.real()) * m_settings.m_colourSubcarrierLevel;
    sample = Y * m_spanLevel + m_blackLevel + chroma * m_spanLevel;
} else {
    // Grayscale: read Y from BGR mat
    const cv::Vec3b& px = m_image.at<cv::Vec3b>(m_imageLine, pointIndex);
    sample = (0.299f * px[2] + 0.587f * px[1] + 0.114f * px[0]) / 255.0f
             * m_spanLevel + m_blackLevel;
}
```

**PAL V alternation:** Uses `m_lineCount & 1` (line parity within frame), not a separate toggle flag. This is simpler and matches hacktv's `pal` variable which alternates based on line number.

---

## Section 6 — Burst injection in back porch (CORRECTED)

**File:** `atvmodsource.h`, `pullImageSample()` — back porch region

This is where the most critical bugs were found and fixed.

### Compositing formula

The burst is rendered as:
```cpp
sample = m_blackLevel + burstLvl * m_spanLevel * envelope * (burstI * sc.real() + burstQ * sc.imag());
```

Where `sc = Complex(cos(wt), sin(wt))`, so this expands to:
```
burstI * cos(wt) + burstQ * sin(wt)
```

### Correct burst I/Q values

**PAL burst at +135 degrees** (positive line):
- Target waveform: `sin(wt + 135)` = `sin(135)*cos(wt) + cos(135)*sin(wt)` = `+0.707*cos(wt) - 0.707*sin(wt)`
- Therefore: `burstI = +0.70711`, `burstQ = -0.70711`

**PAL burst at -135 degrees** (negative line):
- Target waveform: `sin(wt - 135)` = `-0.707*cos(wt) - 0.707*sin(wt)`
- Therefore: `burstI = -0.70711`, `burstQ = -0.70711`

**NTSC burst at 180 degrees:**
- Target waveform: `sin(wt + 180)` = `-sin(wt)`
- Therefore: `burstI = 0`, `burstQ = -1`

### Corrected code

```cpp
float burstI, burstQ;
if (m_settings.m_colourStd == ATVModSettings::ATVColourNTSC) {
    burstI = 0.0f; burstQ = -1.0f;  // burst at 180 deg: -sin(wt)
} else {
    bool palBurstPositive = !(m_lineCount & 1);
    burstI = palBurstPositive ? 0.70711f : -0.70711f;  // cos coeff: +/-sin(135)
    burstQ = -0.70711f;  // sin coeff: cos(135), always negative
}
// Burst amplitude is HALF the peak-to-peak spec value
float burstLvl = (m_settings.m_colourStd == ATVModSettings::ATVColourNTSC)
                 ? (4.0f/20.0f) : (3.0f/14.0f);
sample = m_blackLevel
       + burstLvl * m_spanLevel * envelope * (burstI * sc.real() + burstQ * sc.imag());
```

### What was wrong in the initial implementation

| Value | Initial (WRONG) | Corrected | Why wrong |
|-------|-----------------|-----------|-----------|
| PAL burstI | always `-0.70711` | `+/-0.70711` (alternating) | I was fixed but should alternate with burst sign |
| PAL burstQ | `+/-0.70711` (alternating) | always `-0.70711` | Q was alternating but should be fixed |
| NTSC burstI | `-1.0` | `0.0` | Swapped with burstQ |
| NTSC burstQ | `0.0` | `-1.0` | Swapped with burstI |
| PAL burstLvl | `3/7` | `3/14` | Used p-p value instead of amplitude |
| NTSC burstLvl | `4/10` | `4/20` | Used p-p value instead of amplitude |

The net effect was a 180-degree phase error: the decoder negated both U and V chrominance,
producing complementary (wrong) colours (e.g., red appeared as cyan, blue as yellow).

### Verification against hacktv

hacktv's compositing (`video.c:3537`):
```c
*o += (l->lut[x].i * oc[1] * pal + l->lut[x].q * oc[0]) >> 15;
```
Where `lut.i = cos(wt)`, `lut.q = sin(wt)`, `oc[0] = burst_phase.i * window`, `oc[1] = burst_phase.q * window`.

hacktv's PAL burst_phase (`video.c:4536-4539`):
```c
s->burst_phase.i = round(cos(135.0 * (M_PI / 180.0)) * INT16_MAX);  // = -0.70711 * 32767
s->burst_phase.q = round(sin(135.0 * (M_PI / 180.0)) * INT16_MAX);  // = +0.70711 * 32767
```

Expanding hacktv for PAL positive (pal=+1):
```
cos(wt) * burst_phase.q * pal + sin(wt) * burst_phase.i
= cos(wt) * 0.707 * (+1) + sin(wt) * (-0.707)
= +0.707*cos(wt) - 0.707*sin(wt)
= sin(wt + 135)  [correct]
```

Our corrected version:
```
burstI * cos(wt) + burstQ * sin(wt)
= 0.707 * cos(wt) + (-0.707) * sin(wt)
= +0.707*cos(wt) - 0.707*sin(wt)
= sin(wt + 135)  [matches hacktv]
```

---

## Section 6b — Burst on VBI black lines

**File:** `atvmodsource.h`, `pullBlackLineSample()`

The `LineBlack` lines during VBI (lines 5-21 field 1, 318-334 field 2 for PAL 625) originally
output only sync + flat black — no colour burst. This prevented the decoder's PLL from locking
to the subcarrier phase before the first visible line, causing colour artifacts at the top of
the frame.

**Fix:** Added burst during the back porch of black lines, identical to the burst in
`pullImageSample()`:

```cpp
inline void pullBlackLineSample(Real& sample)
{
    if (m_horizontalCount < m_pointsPerSync)
    {
        sample = 0.0f; // sync
    }
    else if (m_horizontalCount < m_pointsPerSync + m_pointsPerBP)
    {
        // Colour burst during back porch — lets decoder lock before visible lines
        if (m_settings.m_colourEnabled
            && m_colourLUTWidth > 0
            && m_horizontalCount >= m_burstLeftPoints
            && m_horizontalCount <  m_burstLeftPoints + m_burstWidthPoints)
        {
            // [same burst code as pullImageSample]
        }
        else
        {
            sample = m_blackLevel;
        }
    }
    else
    {
        sample = m_blackLevel;
    }
}
```

This gives the decoder ~17 lines of burst to lock onto before the first visible line.

---

## Section 7 — Per-line colour state update

**File:** `atvmodsource.cpp`, `pullVideo()`, at line-end and frame-end

At end of each line (when `m_horizontalCount` wraps):
```cpp
if (m_settings.m_colourEnabled && m_colourLUTWidth > 0) {
    m_colourLUTOffset = (m_colourLUTOffset + m_pointsPerLine) % m_colourLUTWidth;
}
```

At start of each new frame (same block, `m_lineCount` wraps to 0):
```cpp
if (m_settings.m_colourEnabled && m_colourLUTWidth > 0) {
    m_colourLUTOffset = (m_colourLUTOffset + m_pointsPerLine) % m_colourLUTWidth;
    m_frameCount++;
}
```

**Note:** `m_colourLUTOffset` is NOT reset at frame boundaries — it must be continuous across
frames. The LUT is sized to cover the full 4-frame PAL (or 2-frame NTSC) period, ensuring
the subcarrier phase naturally repeats.

---

## Section 8 — applySettings wiring

**File:** `atvmodsource.cpp`, `applySettings()`

```cpp
if ((settingsKeys.contains("colourEnabled") && (settings.m_colourEnabled != m_settings.m_colourEnabled))
    || (settingsKeys.contains("colourStd") && (settings.m_colourStd != m_settings.m_colourStd)))
{
    applyStandard(settings);
    if (getMessageQueueToGUI()) {
        ATVModReport::MsgReportEffectiveSampleRate *report =
            ATVModReport::MsgReportEffectiveSampleRate::create(m_tvSampleRate, m_pointsPerLine);
        getMessageQueueToGUI()->push(report);
    }
}
```

`colourSubcarrierLevel` does not trigger LUT rebuild — it is read directly from `m_settings`
in the hot path on every sample.

---

## Section 9 — Colour bars test pattern

**File:** `atvmodsource.cpp` — static data:

```cpp
// SMPTE 75% colour bars: White, Yellow, Cyan, Green, Magenta, Red, Blue
const ATVModSource::ColourBar ATVModSource::m_colourBars[7] = {
    { 1.000f,  0.000f,  0.000f }, // White   (1,1,1)
    { 0.886f, -0.437f,  0.100f }, // Yellow  (1,1,0)
    { 0.701f,  0.147f, -0.615f }, // Cyan    (0,1,1)
    { 0.587f, -0.289f, -0.515f }, // Green   (0,1,0)
    { 0.413f,  0.289f,  0.515f }, // Magenta (1,0,1)
    { 0.299f, -0.147f,  0.615f }, // Red     (1,0,0)
    { 0.114f,  0.437f, -0.100f }, // Blue    (0,0,1)
};
```

**File:** `atvmodsource.h`, inside `pullImageSample()`:

```cpp
case ATVModSettings::ATVModInputColorBars:
{
    int barIndex = (pointIndex * 7) / m_pointsPerImgLine;
    barIndex = std::clamp(barIndex, 0, 6);
    const ColourBar& bar = m_colourBars[barIndex];
    if (m_settings.m_colourEnabled && m_colourLUTWidth > 0) {
        float Vmod = (m_settings.m_colourStd == ATVModSettings::ATVColourPAL
                      && (m_lineCount & 1)) ? -bar.V : bar.V;
        uint32_t lutIdx = (m_colourLUTOffset + (uint32_t)m_horizontalCount) % m_colourLUTWidth;
        const Complex& sc = m_colourLUT[lutIdx];
        float chroma = (bar.U * sc.imag() + Vmod * sc.real()) * m_settings.m_colourSubcarrierLevel;
        sample = bar.Y * m_spanLevel + m_blackLevel + chroma * m_spanLevel;
    } else {
        sample = bar.Y * m_spanLevel + m_blackLevel;
    }
    break;
}
```

---

## Section 10 — GUI controls

**File:** `atvmodgui.ui`

Added to the layout (after existing controls, before the bottom line separator):
- `QCheckBox` `checkBoxColour` — "Colour" label, tooltip about PAL/NTSC encoding
- `QComboBox` `comboBoxColourStd` — items: "PAL", "NTSC"; disabled when checkbox unchecked
- `QSlider` `sliderChromaLevel` — range 0-100, default 100, horizontal
- `QLabel` `chromaLevelText` — shows slider value
- Added "Color Bars" item to the input source combo box

Also modified:
- FM excursion dial `maximum`: **500 -> 2000** (allows fmExcursion up to 2.0 instead of 0.5)
- `fmExcursionText` width: **30 -> 40** pixels (accommodates wider numbers)

**File:** `atvmodgui.h` — 3 slot declarations

**File:** `atvmodgui.cpp`:
- `on_checkBoxColour_toggled()` — sets `m_colourEnabled`, enables/disables combo
- `on_comboBoxColourStd_currentIndexChanged()` — sets `m_colourStd`
- `on_sliderChromaLevel_valueChanged()` — sets `m_colourSubcarrierLevel = value / 100.0f`
- `displaySettings()` — restores all colour controls from settings
- `makeUIConnections()` — connects all three signals

---

## Section 11 — WebAPI + Swagger

**File:** `swagger/sdrangel/api/swagger/include/ATVMod.yaml` — 3 new properties:

```yaml
colourEnabled:
  type: integer
colourStd:
  type: integer
colourSubcarrierLevel:
  type: number
  format: float
```

**File:** `swagger/sdrangel/code/qt5/client/SWGATVModSettings.h/.cpp` — hand-edited (swagger-codegen
not available). Added getters/setters/members for the 3 fields.

**File:** `plugins/channeltx/modatv/atvmod.cpp`:
- `webapiUpdateChannelSettings()` — reads 3 colour fields from SWG response
- `webapiFormatChannelSettings()` — writes 3 colour fields to SWG response (both overloads)

---

## Lessons Learned

### 1. Burst phase is the most critical parameter
A 180-degree burst phase error doesn't prevent colour detection — the decoder still locks to the
burst and displays colours. But all hues are inverted (complementary colours). This makes it
look like the encoder "almost works" but the colours are wrong, which is hard to diagnose without
understanding the PAL/NTSC colour phase decoding.

### 2. Peak-to-peak vs amplitude
Broadcast specs give burst level as peak-to-peak ratios. The actual amplitude used in the
sinusoidal burst must be half the p-p value.

### 3. Float precision matters for large LUTs
The colour LUT can have >3M entries. Computing `cos(phaseInc * i)` where `i` is a large integer
and `phaseInc` is a small float loses precision due to float multiplication. Using `double`
for the phase accumulation is essential.

### 4. VBI lines need burst for decoder PLL lock
The decoder's colour PLL needs several lines of burst to lock. If burst only appears starting
at the first visible line, the first few lines will have colour artifacts as the PLL settles.
Adding burst to VBI black lines (which are non-visible) solves this.

### 5. FM deviation affects colour viability
For FM-modulated ATV, the FM excursion must be high enough that the colour burst (which is a
small signal riding on the black level) produces enough FM deviation to be detected by the
receiver's discriminator. Too low FM excursion and the burst falls below the colour killer
threshold, regardless of the chroma amplitude setting.

---

## Future Enhancements

- **Chrominance bandwidth filter:** A 1.4 MHz Gaussian FIR applied to the chrominance signal
  before compositing would improve spectral cleanliness. Currently omitted; the encoder works
  without it but the chroma bandwidth extends further than spec.
- **SECAM support:** Could be added as a third `ATVColourStd` with frequency-modulated colour.
- **Burst blanking during vsync:** Some specs require no burst during vertical sync pulses.
  Currently burst is present on all back-porch-carrying lines.
