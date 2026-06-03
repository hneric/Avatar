# Crocodile Avatar Asset Pipeline

This pipeline converts a single crocodile avatar PNG into LVGL-ready C assets.

## Input

Put the source image here:

```text
tools/avatar/croc_source.png
```

Use a square PNG with a real alpha channel if possible. The `head_nomouth` profile is calibrated for the newer head-only crocodile image without a mouth.

If the file only has a visible checkerboard background baked into the pixels, it should be cleaned into a real transparent PNG first. The generator can still run, but the checkerboard will become part of the avatar.

## Generate

Recommended on Windows: use the PowerShell generator. It does not require Pillow.

```powershell
cd D:\code\esps31
powershell -ExecutionPolicy Bypass -File tools\avatar\make_croc_avatar_assets.ps1 -Src tools\avatar\croc_source.png -Profile head
```

For the no-mouth crocodile head image, use:

```powershell
cd D:\code\esps31
powershell -ExecutionPolicy Bypass -File tools\avatar\make_croc_avatar_assets.ps1 -Src tools\avatar\croc_source_nomouth.png -Profile head_nomouth
```

To use manually generated mouth images, put five images in `tools/avatar/mouth_sources` and run:

```powershell
cd D:\code\esps31
powershell -ExecutionPolicy Bypass -File tools\avatar\make_croc_avatar_assets.ps1 -Src tools\avatar\croc_source_nomouth.png -Profile head_nomouth -MouthDir tools\avatar\mouth_sources
```

Recommended mouth filenames:

```text
mouth_0.png  closed / idle mouth
mouth_1.png  slightly open mouth
mouth_2.png  medium open mouth
mouth_3.png  large open mouth
mouth_4.png  round "O" mouth
```

When `-MouthDir` is used, the generator removes edge-connected white/checkerboard background, crops each mouth, resizes it for the current 270x270 avatar layout, places it at the calibrated mouth center, and writes the normal LVGL C assets plus preview PNGs.

Alternative: use the Python generator with Pillow installed:

```powershell
cd D:\code\esps31
python tools\avatar\make_croc_avatar_assets.py --src tools\avatar\croc_source.png --profile head
```

If Python reports `Pillow is required`, either run the PowerShell command above or install Pillow into that Python environment with `python -m pip install Pillow`.

## Output

The script writes:

```text
firmware/main/avatar_assets/croc_avatar_assets.c
firmware/main/avatar_assets/croc_avatar_assets.h
firmware/main/avatar_assets/preview/*.png
```

The C assets use `LV_COLOR_FORMAT_RGB565A8`, which supports transparent overlay layers without enabling runtime PNG decoding.

## Layer Model

All layers use the same canvas size, so alignment is deterministic:

- `croc_avatar_base`: neutral body/head. For no-mouth sources this is used as-is.
- `croc_avatar_mouth_0`: resting mouth, a small closed arc used by idle/listening.
- `croc_avatar_mouth_1`: small mouth.
- `croc_avatar_mouth_2`: medium mouth.
- `croc_avatar_mouth_3`: open mouth with tongue.
- `croc_avatar_mouth_4`: wide speaking smile.
- `croc_avatar_blink`: eyelid overlay for blinking.
- `croc_avatar_thinking`: transparent expression layer; the UI keeps the normal face without extra brow strokes.

In LVGL, create one `lv_image` object per layer, align all of them to the same top-left coordinate, and switch only the overlay image source/hidden flag.

## Coordinate Calibration

The coordinates are normalized from a 180x180 image.

For the newer no-mouth crocodile head profile:

- mouth center: `(90, 138)`
- mouth patch: unused
- left eye: `x=35, y=54, w=39, h=44`
- right eye: `x=106, y=54, w=39, h=44`

For the older head-only crocodile profile:

- mouth center: `(90, 128)`
- mouth patch: `x=36, y=102, w=112, h=56`
- left eye: `x=42, y=58, w=34, h=40`
- right eye: `x=105, y=58, w=34, h=40`

For the older full-body profile:

- mouth center: `(88, 83)`
- mouth box: `x=55, y=66, w=70, h=36`
- left eye: `x=44, y=34, w=28, h=28`
- right eye: `x=91, y=34, w=28, h=28`

If a different source image is used, add or adjust a profile in `make_croc_avatar_assets.ps1`.
