param(
    [string]$Src = "tools/avatar/croc_source.png",
    [string]$OutDir = "firmware/main/avatar_assets",
    [int]$Size = 270,
    [ValidateSet("head", "head_nomouth", "full_body")]
    [string]$Profile = "head",
    [string]$MouthDir = "",
    [switch]$KeepBackground
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$canonicalSize = 180.0
$scale = $Size / $canonicalSize

$profiles = @{
    head = @{
        cover_mouth = $false
        mouth_patch = @(36, 102, 148, 158)
        mouth_center = @(90, 132)
        left_eye_box = @(42, 58, 76, 98)
        right_eye_box = @(105, 58, 139, 98)
        patch_fill = @(138, 221, 39, 245)
        patch_highlight = @(176, 243, 59, 150)
        patch_shadow = @(56, 142, 35, 90)
        mouth_scale = 0.92
    }
    head_nomouth = @{
        cover_mouth = $false
        mouth_patch = @(48, 112, 132, 152)
        mouth_center = @(90, 138)
        left_eye_box = @(35, 54, 74, 98)
        right_eye_box = @(106, 54, 145, 98)
        patch_fill = @(138, 221, 39, 245)
        patch_highlight = @(176, 243, 59, 150)
        patch_shadow = @(56, 142, 35, 90)
        mouth_scale = 0.82
    }
    full_body = @{
        cover_mouth = $true
        mouth_patch = @(50, 62, 130, 106)
        mouth_center = @(88, 84)
        left_eye_box = @(43, 31, 74, 63)
        right_eye_box = @(90, 31, 121, 63)
        patch_fill = @(251, 226, 105, 245)
        patch_highlight = @(255, 238, 139, 170)
        patch_shadow = @(132, 182, 49, 80)
        mouth_scale = 1.0
    }
}

function New-Color($rgba) {
    return [System.Drawing.Color]::FromArgb($rgba[3], $rgba[0], $rgba[1], $rgba[2])
}

function Flatten-Values($values) {
    $flat = New-Object System.Collections.Generic.List[object]
    function Add-FlatValue($v) {
        if ($v -is [System.Array]) {
            foreach ($item in $v) {
                Add-FlatValue $item
            }
        } else {
            $flat.Add($v)
        }
    }
    Add-FlatValue $values
    return $flat.ToArray()
}

function Scale-Box($box, [double]$s) {
    $box = Flatten-Values $box
    return @(
        [int][Math]::Round($box[0] * $s),
        [int][Math]::Round($box[1] * $s),
        [int][Math]::Round($box[2] * $s),
        [int][Math]::Round($box[3] * $s)
    )
}

function Scale-Point($point, [double]$s) {
    $point = Flatten-Values $point
    return @(
        [int][Math]::Round($point[0] * $s),
        [int][Math]::Round($point[1] * $s)
    )
}

function New-TransparentBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.Dispose()
    return $bmp
}

function New-BitmapFromFile($path) {
    $src = [System.Drawing.Image]::FromFile((Resolve-Path $path))
    try {
        $bmp = New-Object System.Drawing.Bitmap $src.Width, $src.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $g.DrawImage($src, 0, 0, $src.Width, $src.Height)
        $g.Dispose()
        return $bmp
    } finally {
        $src.Dispose()
    }
}

function New-RoundedPath([System.Drawing.RectangleF]$rect, [double]$radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [float]($radius * 2)
    if ($d -lt 1) {
        $path.AddRectangle($rect)
        return $path
    }
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function Fill-RoundedRect($g, $box, [double]$radius, $color) {
    $rect = New-Object System.Drawing.RectangleF $box[0], $box[1], ($box[2] - $box[0]), ($box[3] - $box[1])
    $path = New-RoundedPath $rect $radius
    $brush = New-Object System.Drawing.SolidBrush $color
    $g.FillPath($brush, $path)
    $brush.Dispose()
    $path.Dispose()
}

function New-ResizedSource($path, [int]$size) {
    $srcBmp = [System.Drawing.Image]::FromFile((Resolve-Path $path))
    try {
        $dst = New-TransparentBitmap $size
        $g = [System.Drawing.Graphics]::FromImage($dst)
        $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.DrawImage($srcBmp, 0, 0, $size, $size)
        $g.Dispose()
        return $dst
    } finally {
        $srcBmp.Dispose()
    }
}

function Test-CheckerPixel([System.Drawing.Color]$c) {
    if ($c.A -eq 0) {
        return $true
    }
    $max = [Math]::Max($c.R, [Math]::Max($c.G, $c.B))
    $min = [Math]::Min($c.R, [Math]::Min($c.G, $c.B))
    return ($c.A -gt 0 -and ($max - $min) -le 22 -and $c.R -ge 165 -and $c.G -ge 165 -and $c.B -ge 165)
}

function Remove-CheckerboardBackground([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width
    $h = $bmp.Height
    $visited = New-Object byte[] ($w * $h)
    $queue = New-Object 'System.Collections.Generic.Queue[int]'

    function Add-BackgroundIndex([int]$idx) {
        if ($idx -lt 0 -or $idx -ge $visited.Length -or $visited[$idx] -ne 0) {
            return
        }
        $x = $idx % $w
        $y = [int][Math]::Floor($idx / $w)
        if (Test-CheckerPixel $bmp.GetPixel($x, $y)) {
            $visited[$idx] = 1
            $queue.Enqueue($idx)
        }
    }

    for ($x = 0; $x -lt $w; $x++) {
        Add-BackgroundIndex $x
        Add-BackgroundIndex (($h - 1) * $w + $x)
    }
    for ($y = 0; $y -lt $h; $y++) {
        Add-BackgroundIndex ($y * $w)
        Add-BackgroundIndex ($y * $w + ($w - 1))
    }

    while ($queue.Count -gt 0) {
        $idx = $queue.Dequeue()
        $x = $idx % $w
        $y = [int][Math]::Floor($idx / $w)
        $p = $bmp.GetPixel($x, $y)
        if ($p.A -ne 0) {
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(0, $p.R, $p.G, $p.B))
        }
        if ($x -gt 0) { Add-BackgroundIndex ($idx - 1) }
        if ($x + 1 -lt $w) { Add-BackgroundIndex ($idx + 1) }
        if ($y -gt 0) { Add-BackgroundIndex ($idx - $w) }
        if ($y + 1 -lt $h) { Add-BackgroundIndex ($idx + $w) }
    }
}

function Find-MouthFile([string]$dir, [int]$variant) {
    $nameSets = @(
        @("mouth_0", "closed", "close", "idle"),
        @("mouth_1", "small", "slight", "micro", "open_small"),
        @("mouth_2", "medium", "mid", "open_medium"),
        @("mouth_3", "large", "big", "open_large"),
        @("mouth_4", "o", "round", "round_o", "open_o")
    )
    $exts = @(".png", ".jpg", ".jpeg", ".webp")
    foreach ($stem in $nameSets[$variant]) {
        foreach ($ext in $exts) {
            $candidate = Join-Path $dir ($stem + $ext)
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }
    return $null
}

function Resize-Bitmap([System.Drawing.Bitmap]$src, [int]$w, [int]$h) {
    $dst = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.DrawImage($src, 0, 0, $w, $h)
    $g.Dispose()
    return $dst
}

function New-SmiledClosedMouth([System.Drawing.Bitmap]$src) {
    $supersample = 3
    $hiSrc = Resize-Bitmap $src ($src.Width * $supersample) ($src.Height * $supersample)
    $padY = 10
    $hiPadY = $padY * $supersample
    $hiDst = New-Object System.Drawing.Bitmap $hiSrc.Width, ($hiSrc.Height + $hiPadY), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($hiDst)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.Dispose()

    $maxDrop = [Math]::Max(4 * $supersample, [int][Math]::Round($hiSrc.Height * 0.58))
    for ($y = 0; $y -lt $hiSrc.Height; $y++) {
        for ($x = 0; $x -lt $hiSrc.Width; $x++) {
            $p = $hiSrc.GetPixel($x, $y)
            if ($p.A -eq 0) {
                continue
            }
            $nx = (([double]$x / [double]([Math]::Max(1, $hiSrc.Width - 1))) * 2.0) - 1.0
            $drop = [int][Math]::Round((1.0 - ($nx * $nx)) * $maxDrop)
            $dy = [Math]::Min($hiDst.Height - 1, $y + $drop)
            $old = $hiDst.GetPixel($x, $dy)
            if ($p.A -ge $old.A) {
                $hiDst.SetPixel($x, $dy, $p)
            }
        }
    }
    $dst = Resize-Bitmap $hiDst $src.Width ($src.Height + $padY)
    $hiSrc.Dispose()
    $hiDst.Dispose()
    return $dst
}

function New-ExternalMouthLayer([int]$size, $profile, [double]$s, [int]$variant, [string]$mouthDir) {
    $path = Find-MouthFile $mouthDir $variant
    if (-not $path) {
        throw "missing external mouth image for mouth_$variant in $mouthDir"
    }
    Write-Host "processing external mouth_$variant from $path"

    $source = New-BitmapFromFile $path
    $raw = $source
    if ($source.Width -gt 640 -or $source.Height -gt 220) {
        $workRatio = [Math]::Min(340.0 / [double]$source.Width, 105.0 / [double]$source.Height)
        $workW = [Math]::Max(1, [int][Math]::Round($source.Width * $workRatio))
        $workH = [Math]::Max(1, [int][Math]::Round($source.Height * $workRatio))
        $raw = Resize-Bitmap $source $workW $workH
        $source.Dispose()
        Write-Host "  resized work image to ${workW}x${workH}"
    }
    try {
        Remove-CheckerboardBackground $raw
        $croppedInfo = Crop-AlphaBounds $raw "external_mouth_$variant"
        try {
            $cropped = $croppedInfo.bitmap
            $target = switch ($variant) {
                0 { @(116, 24, 3) }
                1 { @(92,  36, 2) }
                2 { @(108, 50, 2) }
                3 { @(122, 60, 2) }
                default { @(70, 70, 0) }
            }
            $target = @(Flatten-Values $target)
            $maxW = [Convert]::ToInt32($target[0])
            $maxH = [Convert]::ToInt32($target[1])
            $yShift = [Convert]::ToInt32($target[2])
            if ($variant -eq 0) {
                $maxH = 36
                $yShift = 4
            }

            $ratio = [Math]::Min([double]$maxW / [double]$cropped.Width, [double]$maxH / [double]$cropped.Height)
            $newW = [Math]::Max(1, [int][Math]::Round($cropped.Width * $ratio))
            $newH = [Math]::Max(1, [int][Math]::Round($cropped.Height * $ratio))
            $resized = Resize-Bitmap $cropped $newW $newH
            try {
                if ($variant -eq 0) {
                    $smiled = New-SmiledClosedMouth $resized
                    $resized.Dispose()
                    $resized = $smiled
                    $newW = $resized.Width
                    $newH = $resized.Height
                }
                $img = New-TransparentBitmap $size
                $g = [System.Drawing.Graphics]::FromImage($img)
                $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
                $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                $center = @(Flatten-Values (Scale-Point -point $profile["mouth_center"] -s $s))
                $cx = [Convert]::ToInt32($center[0])
                $cy = [Convert]::ToInt32($center[1]) + [int]($yShift * $s)
                $g.DrawImage($resized, [int]($cx - $newW / 2), [int]($cy - $newH / 2), $newW, $newH)
                $g.Dispose()
                return $img
            } finally {
                $resized.Dispose()
            }
        } finally {
            $croppedInfo.bitmap.Dispose()
        }
    } finally {
        $raw.Dispose()
    }
}

function New-BaseLayer($srcBmp, $profile, [double]$s) {
    $base = $srcBmp.Clone()
    $g = [System.Drawing.Graphics]::FromImage($base)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    if ($profile.ContainsKey("cover_mouth") -and -not [bool]$profile["cover_mouth"]) {
        $g.Dispose()
        return $base
    }
    $patch = Scale-Box -box $profile["mouth_patch"] -s $s
    Fill-RoundedRect $g $patch (18 * $s) (New-Color $profile["patch_fill"])

    $x0 = $patch[0]; $y0 = $patch[1]; $x1 = $patch[2]; $y1 = $patch[3]
    $highlightBrush = New-Object System.Drawing.SolidBrush (New-Color $profile["patch_highlight"])
    $g.FillEllipse($highlightBrush, $x0 + [int](4 * $s), $y0 + [int](4 * $s), ($x1 - $x0) - [int](8 * $s), ($y1 - $y0) + [int](20 * $s))
    $highlightBrush.Dispose()

    $shadowPen = New-Object System.Drawing.Pen (New-Color $profile["patch_shadow"]), ([Math]::Max(1, [int](2 * $s)))
    $g.DrawArc($shadowPen, $x0 + [int](10 * $s), $y0 + [int](8 * $s), ($x1 - $x0) - [int](20 * $s), ($y1 - $y0) + [int](24 * $s), 190, 150)
    $shadowPen.Dispose()
    $g.Dispose()
    return $base
}

function Crop-AlphaBounds([System.Drawing.Bitmap]$bmp, [string]$name) {
    $minX = $bmp.Width
    $minY = $bmp.Height
    $maxX = -1
    $maxY = -1
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            if ($bmp.GetPixel($x, $y).A -gt 0) {
                if ($x -lt $minX) { $minX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) {
        $empty = New-TransparentBitmap 1
        return @{
            name = $name
            bitmap = $empty
            x = 0
            y = 0
            w = 1
            h = 1
        }
    }
    $pad = 2
    $minX = [Math]::Max(0, $minX - $pad)
    $minY = [Math]::Max(0, $minY - $pad)
    $maxX = [Math]::Min($bmp.Width - 1, $maxX + $pad)
    $maxY = [Math]::Min($bmp.Height - 1, $maxY + $pad)
    $w = $maxX - $minX + 1
    $h = $maxY - $minY + 1
    $cropped = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($cropped)
    $g.Clear([System.Drawing.Color]::Transparent)
    $srcRect = New-Object System.Drawing.Rectangle $minX, $minY, $w, $h
    $dstRect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $g.DrawImage($bmp, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    return @{
        name = $name
        bitmap = $cropped
        x = $minX
        y = $minY
        w = $w
        h = $h
    }
}

function New-MouthLayer([int]$size, $profile, [double]$s, [int]$variant) {
    $img = New-TransparentBitmap $size
    $g = [System.Drawing.Graphics]::FromImage($img)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $center = @(Flatten-Values (Scale-Point -point $profile["mouth_center"] -s $s))
    $cx = [Convert]::ToInt32($center[0])
    $cy = [Convert]::ToInt32($center[1])
    $ms = [double]$s * [double]$profile["mouth_scale"]

    $dark = [System.Drawing.Color]::FromArgb(245, 83, 24, 26)
    $tongue = [System.Drawing.Color]::FromArgb(235, 245, 104, 104)

    if ($variant -eq 0) {
        $restW = [int](62.0 * $ms)
        $restH = [int](13.0 * $ms)
        $box = @(
            [int]($cx - $restW / 2),
            [int]($cy - $restH / 2),
            [int]($cx + $restW / 2),
            [int]($cy + $restH / 2)
        )
        $smilePath = New-Object System.Drawing.Drawing2D.GraphicsPath
        $smilePath.AddBezier(
            [float]$box[0],
            [float]($box[1] + $restH * 0.35),
            [float]($cx - $restW * 0.25),
            [float]($box[3] + $restH * 0.15),
            [float]($cx + $restW * 0.25),
            [float]($box[3] + $restH * 0.15),
            [float]$box[2],
            [float]($box[1] + $restH * 0.35)
        )
        $smilePath.AddBezier(
            [float]$box[2],
            [float]($box[1] + $restH * 0.35),
            [float]($cx + $restW * 0.24),
            [float]($box[3] + $restH * 0.55),
            [float]($cx - $restW * 0.24),
            [float]($box[3] + $restH * 0.55),
            [float]$box[0],
            [float]($box[1] + $restH * 0.35)
        )
        $smilePath.CloseFigure()
        $smileBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(210, 93, 37, 29))
        $g.FillPath($smileBrush, $smilePath)
        $smileBrush.Dispose()
        $smilePath.Dispose()
        $g.Dispose()
        return $img
    }

    $shape = switch ($variant) {
        1 { @(58, 13, 12, $false) }
        2 { @(70, 20, 14, $true) }
        3 { @(82, 28, 16, $true) }
        default { @(88, 30, 16, $true) }
    }
    $shape = @(Flatten-Values $shape)
    $w = [Convert]::ToInt32($shape[0])
    $h = [Convert]::ToInt32($shape[1])
    $radius = [Convert]::ToInt32($shape[2])
    $showTongue = [bool]$shape[3]
    $halfW = [int]([double]$w * $ms / 2.0)
    $halfH = [int]([double]$h * $ms / 2.0)
    $box = @(
        [int]($cx - $halfW),
        [int]($cy - $halfH),
        [int]($cx + $halfW),
        [int]($cy + $halfH)
    )
    $mouthPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $leftX = [float]$box[0]
    $rightX = [float]$box[2]
    $topY = [float]($box[1] + (($box[3] - $box[1]) * 0.18))
    $bottomY = [float]$box[3]
    $mouthPath.AddBezier($leftX, $topY, [float]($cx - $halfW * 0.58), [float]($box[1]), [float]($cx + $halfW * 0.58), [float]($box[1]), $rightX, $topY)
    $mouthPath.AddBezier($rightX, $topY, [float]($cx + $halfW * 0.72), [float]($box[3] + $halfH * 0.12), [float]($cx - $halfW * 0.72), [float]($box[3] + $halfH * 0.12), $leftX, $topY)
    $mouthPath.CloseFigure()
    $mouthBrush = New-Object System.Drawing.SolidBrush $dark
    $g.FillPath($mouthBrush, $mouthPath)
    $mouthBrush.Dispose()
    $mouthPath.Dispose()

    if ($showTongue) {
        $tonguePath = New-Object System.Drawing.Drawing2D.GraphicsPath
        $tongueLeft = [float]($cx - $halfW * 0.38)
        $tongueRight = [float]($cx + $halfW * 0.38)
        $tongueTop = [float]($cy + $halfH * 0.10)
        $tongueBottom = [float]($cy + $halfH * 0.80)
        $tonguePath.AddBezier($tongueLeft, $tongueTop, [float]($cx - $halfW * 0.18), [float]($tongueTop - $halfH * 0.18), [float]($cx + $halfW * 0.18), [float]($tongueTop - $halfH * 0.18), $tongueRight, $tongueTop)
        $tonguePath.AddBezier($tongueRight, $tongueTop, [float]($cx + $halfW * 0.32), $tongueBottom, [float]($cx - $halfW * 0.32), $tongueBottom, $tongueLeft, $tongueTop)
        $tonguePath.CloseFigure()
        $tongueBrush = New-Object System.Drawing.SolidBrush $tongue
        $g.FillPath($tongueBrush, $tonguePath)
        $centerPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(85, 175, 65, 68)), ([Math]::Max(1, [int](1 * $ms)))
        $g.DrawLine($centerPen, [float]$cx, [float]($tongueTop + $halfH * 0.12), [float]$cx, [float]($tongueBottom - $halfH * 0.08))
        $centerPen.Dispose()
        $tongueBrush.Dispose()
        $tonguePath.Dispose()
    }
    $g.Dispose()
    return $img
}

function New-BlinkLayer([int]$size, $profile, [double]$s) {
    $img = New-TransparentBitmap $size
    $g = [System.Drawing.Graphics]::FromImage($img)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $greenBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(245, 127, 209, 47))
    $linePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 22, 79, 26)), ([Math]::Max(2, [int](3 * $s)))
    foreach ($rawBox in @($profile["left_eye_box"], $profile["right_eye_box"])) {
        $box = Scale-Box -box $rawBox -s $s
        $g.FillEllipse($greenBrush, $box[0] - [int](2 * $s), $box[1], ($box[2] - $box[0]) + [int](4 * $s), $box[3] - $box[1])
        $g.DrawArc($linePen, $box[0], $box[1] + [int](8 * $s), $box[2] - $box[0], ($box[3] - $box[1]) - [int](3 * $s), 12, 156)
    }
    $greenBrush.Dispose()
    $linePen.Dispose()
    $g.Dispose()
    return $img
}

function New-ThinkingLayer([int]$size, $profile, [double]$s) {
    return New-TransparentBitmap $size
}

function Convert-ToRgb565A8([System.Drawing.Bitmap]$bmp) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    $alpha = New-Object System.Collections.Generic.List[byte]
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $rgb565 = (($p.R -band 0xF8) -shl 8) -bor (($p.G -band 0xF8) -shl 3) -bor ($p.B -shr 3)
            $bytes.Add([byte]($rgb565 -band 0xFF))
            $bytes.Add([byte](($rgb565 -shr 8) -band 0xFF))
            $alpha.Add([byte]$p.A)
        }
    }
    $bytes.AddRange($alpha)
    return $bytes.ToArray()
}

function Format-CArray([string]$name, [byte[]]$bytes) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("static const uint8_t ${name}_map[] = {")
    for ($i = 0; $i -lt $bytes.Length; $i += 16) {
        $chunk = @()
        for ($j = $i; $j -lt [Math]::Min($i + 16, $bytes.Length); $j++) {
            $chunk += ("0x{0:x2}" -f $bytes[$j])
        }
        $lines.Add("    " + ($chunk -join ",") + ",")
    }
    $lines.Add("};")
    return $lines -join "`n"
}

function Format-ImageDsc([string]$name, [int]$width, [int]$height) {
@"
const lv_image_dsc_t $name = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_RGB565A8,
  .header.flags = 0,
  .header.w = $width,
  .header.h = $height,
  .header.stride = $($width * 2),
  .data_size = sizeof(${name}_map),
  .data = ${name}_map,
};
"@
}

if (-not (Test-Path -LiteralPath $Src)) {
    throw "source image not found: $Src"
}

$profileData = $profiles[$Profile]
$useExternalMouths = ($MouthDir -ne "")
if ($useExternalMouths -and -not (Test-Path -LiteralPath $MouthDir)) {
    throw "mouth image directory not found: $MouthDir"
}
$previewDir = Join-Path $OutDir "preview"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $previewDir | Out-Null

$srcBmp = New-ResizedSource $Src $Size
if (-not $KeepBackground) {
    Remove-CheckerboardBackground $srcBmp
}
$layers = [ordered]@{
    croc_avatar_base = New-BaseLayer $srcBmp $profileData $scale
    croc_avatar_mouth_0 = $(if ($useExternalMouths) { New-ExternalMouthLayer $Size $profileData $scale 0 $MouthDir } else { New-MouthLayer $Size $profileData $scale 0 })
    croc_avatar_mouth_1 = $(if ($useExternalMouths) { New-ExternalMouthLayer $Size $profileData $scale 1 $MouthDir } else { New-MouthLayer $Size $profileData $scale 1 })
    croc_avatar_mouth_2 = $(if ($useExternalMouths) { New-ExternalMouthLayer $Size $profileData $scale 2 $MouthDir } else { New-MouthLayer $Size $profileData $scale 2 })
    croc_avatar_mouth_3 = $(if ($useExternalMouths) { New-ExternalMouthLayer $Size $profileData $scale 3 $MouthDir } else { New-MouthLayer $Size $profileData $scale 3 })
    croc_avatar_mouth_4 = $(if ($useExternalMouths) { New-ExternalMouthLayer $Size $profileData $scale 4 $MouthDir } else { New-MouthLayer $Size $profileData $scale 4 })
    croc_avatar_blink = New-BlinkLayer $Size $profileData $scale
    croc_avatar_thinking = New-ThinkingLayer $Size $profileData $scale
}

try {
    $assetInfos = [ordered]@{}
    foreach ($entry in $layers.GetEnumerator()) {
        $entry.Value.Save((Join-Path $previewDir "$($entry.Key)_full.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        if ($entry.Key -eq "croc_avatar_base") {
            $assetInfos[$entry.Key] = @{
                name = $entry.Key
                bitmap = $entry.Value.Clone()
                x = 0
                y = 0
                w = $entry.Value.Width
                h = $entry.Value.Height
            }
        } else {
            $assetInfos[$entry.Key] = Crop-AlphaBounds $entry.Value $entry.Key
        }
        $assetInfos[$entry.Key].bitmap.Save((Join-Path $previewDir "$($entry.Key).png"), [System.Drawing.Imaging.ImageFormat]::Png)
    }

    $hLines = New-Object System.Collections.Generic.List[string]
    $hLines.Add("#pragma once")
    $hLines.Add('#include "lvgl.h"')
    $hLines.Add("")
    $hLines.Add("typedef struct {")
    $hLines.Add("    const lv_image_dsc_t *img;")
    $hLines.Add("    int16_t x;")
    $hLines.Add("    int16_t y;")
    $hLines.Add("    int16_t w;")
    $hLines.Add("    int16_t h;")
    $hLines.Add("} croc_avatar_layer_t;")
    $hLines.Add("")
    foreach ($name in $assetInfos.Keys) {
        $hLines.Add("extern const lv_image_dsc_t $name;")
        $hLines.Add("extern const croc_avatar_layer_t ${name}_layer;")
    }
    Set-Content -LiteralPath (Join-Path $OutDir "croc_avatar_assets.h") -Value ($hLines -join "`n") -Encoding UTF8

    $cLines = New-Object System.Collections.Generic.List[string]
    $cLines.Add('#include "croc_avatar_assets.h"')
    $cLines.Add("")
    foreach ($entry in $assetInfos.GetEnumerator()) {
        $info = $entry.Value
        $bmp = $info.bitmap
        $cLines.Add((Format-CArray $entry.Key (Convert-ToRgb565A8 $bmp)))
        $cLines.Add("")
        $cLines.Add((Format-ImageDsc $entry.Key $bmp.Width $bmp.Height))
        $cLines.Add("")
        $cLines.Add("const croc_avatar_layer_t $($entry.Key)_layer = { &$($entry.Key), $($info.x), $($info.y), $($info.w), $($info.h) };")
        $cLines.Add("")
    }
    Set-Content -LiteralPath (Join-Path $OutDir "croc_avatar_assets.c") -Value ($cLines -join "`n") -Encoding UTF8
} finally {
    if (Get-Variable -Name assetInfos -ErrorAction SilentlyContinue) {
        foreach ($entry in $assetInfos.GetEnumerator()) {
            $entry.Value.bitmap.Dispose()
        }
    }
    foreach ($entry in $layers.GetEnumerator()) {
        $entry.Value.Dispose()
    }
    $srcBmp.Dispose()
}

Write-Host "generated $($layers.Count) layers in $OutDir"
