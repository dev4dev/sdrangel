# Plan v2: PAL/NTSC Colour Support for ATVMod Plugin

## Implementation Status

All 11 sections have been implemented:

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

### Implementation Notes

- Sections 3+5 were merged as required (BGR mat + colour-typed pixel reads are atomic)
- The colour LUT is pre-computed in `applyStandard()` covering 4 frames (PAL) or 2 frames (NTSC)
- PAL V-sign and burst alternation use `(m_frameCount + m_lineCount) & 1` (matching hacktv)
- Burst uses raised-cosine envelope (0.3 us rise/fall) to avoid spectral splatter
- Chroma bandwidth filtering (1.4 MHz Gaussian FIR) is marked as TODO for future enhancement
- ColourBars pattern uses pre-computed SMPTE 75% bar YUV values
- SWG generated files were hand-edited (swagger-codegen not available) — regeneration will overwrite

## Context

The SDRangel ATV modulator (`plugins/channeltx/modatv/`) currently transmits black-and-white only —
all colour source material is force-converted to grayscale via `cv::COLOR_RGB2GRAY` before rendering.
The goal is to add proper analogue colour TV encoding (PAL and NTSC) so that colour video files,
still images, and live camera feeds are transmitted with a colour subcarrier that real receivers can
decode.

The reference implementation is `_hacktv/src/video.c` (GPLv3, Philip Heron) — a battle-tested C
implementation of PAL/NTSC/SECAM encoding for HackRF. The architectural techniques are borrowed
from there; the DSP is adapted to SDRangel's float sample pipeline and C++ plugin structure.

**Key numbers (all sourced from hacktv `vid_config_pal_i` / `vid_config_ntsc_m`):**

| Parameter          | PAL value              | NTSC value              |
|--------------------|------------------------|-------------------------|
| Colour carrier     | 4 433 618.75 Hz        | 3 579 545.45 Hz         |
| Carrier rational   | `{17734475, 4}`        | `{39375000, 11}`        |
| Burst position     | 5.6 µs after sync edge | 5.3 µs                  |
| Burst width        | 2.25 µs                | 2.50 µs                 |
| Burst level        | 3/7 of white–blank     | 4/10 of white–blank     |
| PAL burst phase    | ±135° (alternating)    | —                       |
| NTSC burst phase   | 180° (fixed)           | 180°                    |
| Chrominance BW     | 1.4 MHz (Gaussian)     | 1.4 MHz                 |
| V coefficient (Ev) | 0.877                  | 0.877                   |
| U coefficient (Eu) | 0.493                  | 0.493                   |
| LUT frame period   | 4 frames               | 2 frames                |

**Files to modify:**
- `plugins/channeltx/modatv/atvmodsettings.h/.cpp`
- `plugins/channeltx/modatv/atvmodsource.h/.cpp`
- `plugins/channeltx/modatv/atvmodgui.ui`
- `plugins/channeltx/modatv/atvmodgui.h/.cpp`
- `plugins/channeltx/modatv/atvmodwebapiadapter.h/.cpp`
- `swagger/sdrangel/api/swagger/swagger.yaml` (add new fields; then regenerate client)

---

## Section 1 — Settings: new colour fields

**File:** `atvmodsettings.h`

Add inside `ATVModSettings`:

```cpp
typedef enum {
    ATVColourPAL,   // PAL subcarrier 4433618.75 Hz, ±135° burst, V alternates per line
    ATVColourNTSC   // NTSC subcarrier 3579545.45 Hz, 180° burst fixed
} ATVColourStd;

bool          m_colourEnabled;    // false = grayscale (existing behaviour)
ATVColourStd  m_colourStd;        // PAL or NTSC
float         m_colourSubcarrierLevel; // 0.0–1.0 scale for chroma amplitude, default 1.0
```

**File:** `atvmodsettings.cpp`

- `resetToDefaults()`: `m_colourEnabled = false; m_colourStd = ATVColourPAL; m_colourSubcarrierLevel = 1.0f;`
- `serialize()`: append three new fields at the **next available index** (do not re-use existing indices — check the last used index in the existing `serialize()` and continue from there).
- `deserialize()`: read with the same indices; if absent, fall back to defaults.
- `applySettings(keys, src)`: handle `"colourEnabled"`, `"colourStd"`, `"colourSubcarrierLevel"`.
- `getDebugString()`: print the three new fields when present in keys.

No other files change in this section. Build must compile cleanly with `-DSERVER_MODE` too (no GUI).

---

## Section 2 — Colour subcarrier LUT: allocation and population

**File:** `atvmodsource.h`

Add private members:

```cpp
// Colour subcarrier lookup table (Complex = float {real=cos, imag=sin})
std::vector<Complex> m_colourLUT;   // pre-computed subcarrier waveform
uint32_t  m_colourLUTWidth;         // length of LUT in samples
uint32_t  m_colourLUTOffset;        // current position in LUT (advances per line)
bool      m_palBurstSign;           // PAL ±135° alternation toggle, flips every line
int       m_burstLeftPoints;        // start of burst window in samples from line start
int       m_burstWidthPoints;       // duration of burst window in samples
```

**File:** `atvmodsource.cpp`, inside `applyStandard()` (called whenever standard/sample-rate changes)

Add a block after the existing point-count calculations, gated on `settings.m_colourEnabled`:

```cpp
if (settings.m_colourEnabled)
{
    // Subcarrier frequency
    float fsc = (settings.m_colourStd == ATVModSettings::ATVColourNTSC)
        ? 3579545.4545f   // 39375000/11
        : 4433618.75f;    // 17734475/4

    // LUT covers the full colour-frame repetition period:
    //   PAL: 4 TV frames  (after 4 frames the subcarrier phase is back to 0)
    //   NTSC: 2 TV frames
    int framesInPeriod = (settings.m_colourStd == ATVModSettings::ATVColourNTSC) ? 2 : 4;
    m_colourLUTWidth = (uint32_t)(framesInPeriod) * (uint32_t)m_nbLines * m_pointsPerLine;

    m_colourLUT.resize(m_colourLUTWidth + m_pointsPerLine); // +1 line guard as in hacktv
    float phaseInc = 2.0f * (float)M_PI * fsc / (float)m_tvSampleRate;
    for (uint32_t i = 0; i < m_colourLUT.size(); i++) {
        m_colourLUT[i] = Complex(std::cos(phaseInc * i), std::sin(phaseInc * i));
    }

    m_colourLUTOffset = 0;
    m_palBurstSign    = true;

    // Burst geometry (from hacktv vid_config_pal_i / vid_config_ntsc_m)
    // burst_left  PAL=5.6µs, NTSC=5.3µs; burst_width PAL=2.25µs, NTSC=2.50µs
    float burstLeftUs  = (settings.m_colourStd == ATVModSettings::ATVColourNTSC) ? 5.3e-6f  : 5.6e-6f;
    float burstWidthUs = (settings.m_colourStd == ATVModSettings::ATVColourNTSC) ? 2.5e-6f  : 2.25e-6f;
    float lineUs = 1.0f / ((float)m_settings.m_nbLines * (float)m_settings.m_fps);
    m_burstLeftPoints  = (int)(burstLeftUs  / lineUs * m_pointsPerLine);
    m_burstWidthPoints = (int)(burstWidthUs / lineUs * m_pointsPerLine);
}
else
{
    m_colourLUT.clear();
    m_colourLUTWidth = 0;
}
```

Also reset `m_colourLUTOffset = 0; m_palBurstSign = true;` in the constructor initializer list.

After this section: LUT is ready; burst geometry is in samples. No rendering code changes yet.

---

## Section 3 — Frame storage: load BGR instead of grayscale

**File:** `atvmodsource.h`

Change all `cv::Mat` member types that store image/video frames — they were implicitly `CV_8U`
(1-channel). They remain `cv::Mat` but will now hold `CV_8UC3` (BGR) when colour is active.
No type annotation change needed; OpenCV is dynamic.

**File:** `atvmodsource.cpp`

`openImage()` — change imread flag:
```cpp
// Before:
m_imageFromFile = cv::imread(qPrintable(fileName), cv::ImreadModes::IMREAD_GRAYSCALE);
// After:
m_imageFromFile = cv::imread(qPrintable(fileName), cv::ImreadModes::IMREAD_COLOR); // always BGR
```
The grayscale path in the pixel loop will still work by reading only the B channel (all equal for
gray images). No information is lost.

`pullVideo()` — video file path (~line 404):
```cpp
// Before:
cv::cvtColor(colorFrame, m_videoframeOriginal, cv::COLOR_RGB2GRAY);
// After:
m_videoframeOriginal = colorFrame; // keep BGR; pixel loop handles both modes
```

`pullVideo()` — camera path (~line 531):
```cpp
// Before:
cv::cvtColor(colorFrame, camera.m_videoframeOriginal, cv::COLOR_RGB2GRAY);
// After:
camera.m_videoframeOriginal = colorFrame;
```

`resizeImage()`, `resizeVideo()`, `resizeCamera()`, `resizeCameras()`: **no change needed** —
`cv::resize()` works for any channel count.

`mixImageAndText()` (~line 923): **no change needed** — `cv::putText()` with `cv::Scalar::all()`
works for both 1- and 3-channel images. Text will appear white/gray.

After this section: all frames stored as BGR. Grayscale rendering still works because
`m_image.at<unsigned char>()` on a 3-channel mat will silently read the first byte of each pixel
(B channel). A follow-up section fixes the pixel read to be properly grayscale when colour is off.

---

## Section 4 — Inline RGB→YUV helper

**File:** `atvmodsource.h`, private section

Add a static inline helper (no new file needed):

```cpp
// Coefficients from hacktv vid_config_pal_i / vid_config_ntsc_m
// rw_co=0.299, gw_co=0.587, bw_co=0.114  (ITU-R BT.601)
// eu_co=0.493 (U = Eu * (B-Y)), ev_co=0.877 (V = Ev * (R-Y))
static inline void bgrToYUV(const cv::Vec3b& bgr, float& Y, float& U, float& V)
{
    float r = bgr[2] / 255.0f;
    float g = bgr[1] / 255.0f;
    float b = bgr[0] / 255.0f;
    Y = 0.299f * r + 0.587f * g + 0.114f * b;
    U = 0.493f * (b - Y);   // eu_co
    V = 0.877f * (r - Y);   // ev_co
}
```

This replaces the 16 M-entry lookup table hacktv uses. At 3 multiplies + 2 subtracts per pixel,
it is fast enough for SDRangel's sample rates and avoids 96 MB of memory.

---

## Section 5 — Hot path: colour pixel rendering

**File:** `atvmodsource.h`, `pullImageSample()` inline function

This is the core change. The existing pixel read block (lines 485–533) handles
`ATVModInputImage`, `ATVModInputVideo`, `ATVModInputCamera`. For each of those, add a colour branch.

**Grayscale path (existing, minor fix for Section 3 compatibility):**

```cpp
// Fix grayscale read for BGR mats (was at<unsigned char>, now read Y properly):
const cv::Vec3b& px = m_image.at<cv::Vec3b>(m_imageLine, pointIndex);
float Y = 0.299f * px[2]/255.0f + 0.587f * px[1]/255.0f + 0.114f * px[0]/255.0f;
sample = Y * m_spanLevel + m_blackLevel;
```

**Colour path (new branch, inside `if (m_settings.m_colourEnabled)`):**

```cpp
float Y, U, V;
bgrToYUV(m_image.at<cv::Vec3b>(m_imageLine, pointIndex), Y, U, V);

// PAL: V alternates sign on each line
float Vmod = (m_settings.m_colourStd == ATVModSettings::ATVColourPAL && !m_palBurstSign)
             ? -V : V;

// Subcarrier phase at this sample: LUT index wraps in the render loop
uint32_t lutIdx = (m_colourLUTOffset + (uint32_t)m_horizontalCount) % m_colourLUTWidth;
const Complex& sc = m_colourLUT[lutIdx];

// Composite = Y + U*sin(phase) + V*cos(phase)  — U modulates sine (quadrature), V modulates cosine
float chroma = (U * sc.imag() + Vmod * sc.real()) * m_settings.m_colourSubcarrierLevel;
sample = Y * m_spanLevel + m_blackLevel + chroma * m_spanLevel;
```

Apply the same pattern inside the `ATVModInputVideo` and `ATVModInputCamera` branches, reading
from `m_videoFrame` and `camera.m_videoFrame` respectively (both now BGR after Section 3).

**Test patterns** (Uniform, HBars, VBars, Chessboard, Gradient, Diagonal): these produce a `float`
luma value only. When `m_colourEnabled` is true, pass them through as Y with U=V=0 (no chroma).
No change needed in those branches; they remain grayscale even in colour mode (reasonable for test
signals).

---

## Section 6 — Burst injection in back porch

**File:** `atvmodsource.h`, `pullImageSample()` inline function

Replace the back-porch section (currently outputs `m_blackLevel` unconditionally):

```cpp
// Back porch region: m_horizontalCount in [m_pointsPerSync, m_pointsPerSync + m_pointsPerBP)
else if (m_horizontalCount < m_pointsPerSync + m_pointsPerBP)
{
    if (m_settings.m_colourEnabled
        && m_horizontalCount >= m_burstLeftPoints
        && m_horizontalCount <  m_burstLeftPoints + m_burstWidthPoints)
    {
        uint32_t lutIdx = (m_colourLUTOffset + (uint32_t)m_horizontalCount) % m_colourLUTWidth;
        const Complex& sc = m_colourLUT[lutIdx];

        // PAL burst: ±135°.  cos(135°) = -√2/2,  sin(±135°) = ±√2/2
        // NTSC burst: 180°.  cos(180°) = -1,      sin(180°) = 0
        float burstI, burstQ;
        if (m_settings.m_colourStd == ATVModSettings::ATVColourNTSC) {
            burstI = -1.0f; burstQ = 0.0f;
        } else {
            burstI = -0.70711f;                             // cos(135°)
            burstQ =  m_palBurstSign ? 0.70711f : -0.70711f; // sin(±135°)
        }
        // burst_level: PAL = 3/7, NTSC = 4/10  of span
        float burstLvl = (m_settings.m_colourStd == ATVModSettings::ATVColourNTSC)
                         ? (4.0f/10.0f) : (3.0f/7.0f);
        sample = m_blackLevel
               + burstLvl * m_spanLevel * (burstI * sc.real() + burstQ * sc.imag());
    }
    else
    {
        sample = m_blackLevel;
    }
}
```

Note: `m_burstLeftPoints` is already measured from the start of the line (sample 0). The existing
`m_pointsPerSync + m_pointsPerBP` boundary is used to guard the outer else-if as before.

---

## Section 7 — Per-line colour state update

**File:** `atvmodsource.cpp`, `pullVideo()`, at the point where `m_horizontalCount` resets and
`m_lineCount` increments (the `else` branch around line 354).

After advancing `m_lineCount` and recalculating `m_lineType`, add:

```cpp
if (m_settings.m_colourEnabled && m_colourLUTWidth > 0)
{
    // Advance subcarrier phase by one line's worth of samples
    m_colourLUTOffset = (m_colourLUTOffset + m_pointsPerLine) % m_colourLUTWidth;

    // PAL: toggle V-sign every line (burst phase alternates ±135°)
    // NTSC: no alternation needed (burst is always 180°)
    if (m_settings.m_colourStd == ATVModSettings::ATVColourPAL) {
        m_palBurstSign = !m_palBurstSign;
    }
}
```

Also reset both at the start of each new frame (where `m_lineCount` wraps to 0 and `m_imageLine`
resets): **do not reset** `m_colourLUTOffset` here — it must be continuous across frames. Only
reset `m_palBurstSign` on a full frame boundary if desired for alignment, but since the LUT
already covers the full 4-frame PAL period the sign will naturally track correctly as long as it
starts at `true` and is toggled consistently.

---

## Section 8 — applySettings wiring

**File:** `atvmodsource.cpp`, `applySettings()`

Add a guard block so that changes to `colourEnabled`, `colourStd`, or `colourSubcarrierLevel`
trigger re-computation of the LUT:

```cpp
if (settingsKeys.contains("colourEnabled")
    || settingsKeys.contains("colourStd")
    || force)
{
    // Re-run applyStandard to rebuild the colour LUT with new settings
    applyStandard(settings);
    // Report new effective sample rate to GUI (applyStandard may not call this)
    if (getMessageQueueToGUI()) {
        ATVModReport::MsgReportEffectiveSampleRate *report =
            ATVModReport::MsgReportEffectiveSampleRate::create(m_tvSampleRate, m_pointsPerLine);
        getMessageQueueToGUI()->push(report);
    }
}
```

`colourSubcarrierLevel` does not need LUT rebuild — it is read directly in the hot path from
`m_settings.m_colourSubcarrierLevel` on every sample.

---

## Section 9 — Colour bars test pattern

**File:** `atvmodsettings.h`

Add to `ATVModInput` enum:
```cpp
ATVModInputColorBars  // 7-bar SMPTE colour bars
```

**File:** `atvmodsource.h`, private members:

```cpp
// Pre-computed colour bar pixel data (7 bars x {Y,U,V})
struct ColourBar { float Y, U, V; };
static const ColourBar m_colourBars[7]; // defined in .cpp
```

**File:** `atvmodsource.cpp`:

```cpp
// SMPTE 75% colour bars: White, Yellow, Cyan, Green, Magenta, Red, Blue
// Computed from bgrToYUV({255,255,255}), etc.
const ATVModSource::ColourBar ATVModSource::m_colourBars[7] = {
    { 1.000f,  0.000f,  0.000f }, // White
    { 0.886f, -0.436f,  0.100f }, // Yellow  (R=255,G=255,B=0)
    { 0.701f,  0.057f, -0.615f }, // Cyan    (R=0,G=255,B=255)
    { 0.587f, -0.380f, -0.515f }, // Green   (R=0,G=255,B=0)
    { 0.413f,  0.380f,  0.515f }, // Magenta (R=255,G=0,B=255)
    { 0.299f, -0.057f,  0.615f }, // Red     (R=255,G=0,B=0)
    { 0.114f,  0.436f, -0.100f }, // Blue    (R=0,G=0,B=255)
};
```

(Values are pre-calculated; verify with the `bgrToYUV()` helper from Section 4.)

In `pullImageSample()`, add a new case in the switch:

```cpp
case ATVModSettings::ATVModInputColorBars:
{
    int barIndex = (pointIndex * 7) / m_pointsPerImgLine; // 0..6
    barIndex = std::clamp(barIndex, 0, 6);
    const ColourBar& bar = m_colourBars[barIndex];
    if (m_settings.m_colourEnabled) {
        float Vmod = (m_settings.m_colourStd == ATVModSettings::ATVColourPAL && !m_palBurstSign)
                     ? -bar.V : bar.V;
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

In the signal settings group (section A), add below the existing controls:

- `QCheckBox` named `checkBoxColour`, label "Colour"
- `QComboBox` named `comboBoxColourStd`, items: `["PAL", "NTSC"]`, enabled only when checkbox is checked
- `QSlider` (horizontal) named `sliderChromaLevel`, range 0–100 (maps to 0.0–1.0), label "Chroma"

**File:** `atvmodgui.h`

Declare slots:
```cpp
void on_checkBoxColour_toggled(bool checked);
void on_comboBoxColourStd_currentIndexChanged(int index);
void on_sliderChromaLevel_valueChanged(int value);
```

**File:** `atvmodgui.cpp`

In `displaySettings()`:
```cpp
ui->checkBoxColour->setChecked(m_settings.m_colourEnabled);
ui->comboBoxColourStd->setCurrentIndex((int) m_settings.m_colourStd);
ui->comboBoxColourStd->setEnabled(m_settings.m_colourEnabled);
ui->sliderChromaLevel->setValue((int)(m_settings.m_colourSubcarrierLevel * 100));
```

Slot implementations follow the standard `applySettings` + `sendSettings` pattern used by all
other controls in this file. For example:
```cpp
void ATVModGUI::on_checkBoxColour_toggled(bool checked)
{
    m_settings.m_colourEnabled = checked;
    ui->comboBoxColourStd->setEnabled(checked);
    applySettings({"colourEnabled"});
}
```

Also add `ATVModInputColorBars` to the input source combobox (section A.10 combo).

---

## Section 11 — WebAPI adapter

**File:** `atvmodwebapiadapter.cpp`

In `webapiSettingsGet()` (fill SWG from settings):
```cpp
response.getAtvModSettings()->setColourEnabled(settings.m_colourEnabled ? 1 : 0);
response.getAtvModSettings()->setColourStd((int) settings.m_colourStd);
response.getAtvModSettings()->setColourSubcarrierLevel(settings.m_colourSubcarrierLevel);
```

In `webapiSettingsPutPatch()` (fill settings from SWG):
```cpp
if (channelSettingsKeys.contains("colourEnabled")) {
    settings.m_colourEnabled = response.getAtvModSettings()->getColourEnabled() != 0;
}
if (channelSettingsKeys.contains("colourStd")) {
    settings.m_colourStd = (ATVModSettings::ATVColourStd)
                           response.getAtvModSettings()->getColourStd();
}
if (channelSettingsKeys.contains("colourSubcarrierLevel")) {
    settings.m_colourSubcarrierLevel = response.getAtvModSettings()->getColourSubcarrierLevel();
}
```

**File:** `swagger/sdrangel/api/swagger/swagger.yaml`

Locate the `ATVModSettings` object definition and add three new optional properties:
```yaml
colourEnabled:
  type: integer
  description: "1 if colour encoding is enabled, 0 for monochrome"
colourStd:
  type: integer
  description: "Colour standard: 0=PAL, 1=NTSC"
colourSubcarrierLevel:
  type: number
  format: float
  description: "Colour subcarrier amplitude relative to luminance, 0.0–1.0"
```

**Note:** After editing the YAML, regenerate the Qt client:
```bash
cd swagger/sdrangel
/opt/install/swagger/swagger-codegen generate \
  -i api/swagger/swagger.yaml -l qt5cpp \
  -c qt5cpp-config.json -o code/qt5
```
Do not hand-edit files under `swagger/sdrangel/code/qt5/`.

---

## Verification

### Build check
```bash
cmake --preset default -DBUILD_GUI=ON -DBUILD_SERVER=ON
cmake --build --preset default -j$(nproc) 2>&1 | grep -E "error:|warning:"
```
Also build server-only to catch any `#ifdef SERVER_MODE` issues:
```bash
cmake --preset default -DBUILD_GUI=OFF
cmake --build --preset default -j$(nproc) 2>&1 | grep "error:"
```

### Visual functional check
1. Launch SDRangel with a file-based Tx device (SigMF or File sink).
2. Add an ATV Mod channel.
3. Set source to **Color Bars**, enable Colour, select PAL, set sample rate ≥ 8 MS/s.
4. Load the output IQ file into the ATV Demodulator (`plugins/channelrx/demodatv`) or a
   spectrum analyser and verify:
   - A colour subcarrier bump visible in the spectrum ~4.43 MHz above the video carrier.
   - Colour burst visible in each line's back porch (zoomed time-domain view).
5. Set source to **Image** with a colour test image, verify chroma is present in spectrum.
6. Disable colour, verify signal reverts to clean B&W (no subcarrier bump).

### Regression check
Verify existing monochrome operation is unchanged:
- With `m_colourEnabled = false`, the `pullImageSample()` grayscale branch must return identical
  values to the pre-patch code (luma computed from `bgrToYUV().Y` is mathematically equivalent to
  the old `cv::COLOR_*2GRAY` result for Rec.601).

---

## Implementation order

1. Section 1 — Settings (no DSP, compiles immediately, foundation for everything)
2. Section 4 — Inline YUV helper (self-contained, no dependencies)
3. Section 2 — Colour LUT allocation in `applyStandard()` (requires Section 1)
4. Section 3 — Frame storage BGR (safe change, guarded by colour flag at pixel level)
5. Section 5 — Hot-path pixel rendering (requires 1, 2, 3, 4)
6. Section 6 — Burst injection (requires 2)
7. Section 7 — Per-line state update (requires 2)
8. Section 8 — applySettings wiring (requires 1, 2; connects settings changes to DSP rebuild)
9. Section 9 — Colour bars pattern (requires 1, 4, 5)
10. Section 10 — GUI controls (requires 1; independent of DSP sections)
11. Section 11 — WebAPI + Swagger (requires 1; last because Swagger regeneration touches many files)
