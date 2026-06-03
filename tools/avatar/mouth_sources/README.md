Put the five external mouth images in this directory before generating avatar assets.

Recommended filenames:

```text
mouth_0.png  closed / idle mouth
mouth_1.png  slightly open mouth
mouth_2.png  medium open mouth
mouth_3.png  large open mouth
mouth_4.png  round "O" mouth
```

The generator also accepts these aliases:

```text
closed.png, close.png, idle.png
small.png, slight.png, micro.png, open_small.png
medium.png, mid.png, open_medium.png
large.png, big.png, open_large.png
o.png, round.png, round_o.png, open_o.png
```

Generate firmware assets with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\avatar\make_croc_avatar_assets.ps1 -Src tools\avatar\croc_source_nomouth.png -Profile head_nomouth -MouthDir tools\avatar\mouth_sources
```

