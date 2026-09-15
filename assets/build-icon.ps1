$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$frames=@()
foreach($size in @(16,24,32,48,64,128,256)) {
    $bitmap=[Drawing.Bitmap]::new($size,$size)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size/256.0,$size/256.0)
    $dark=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#111827'))
    $teal=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#2DD4BF'))
    $purple=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#A78BFA'))
    $light=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#E2E8F0'))
    try {
        $graphics.FillRectangle($dark,12,40,142,146)
        $graphics.FillRectangle($teal,20,48,126,130)
        $graphics.FillRectangle($dark,30,58,106,110)
        $graphics.FillRectangle($dark,158,72,88,114)
        $graphics.FillRectangle($purple,164,78,76,102)
        $graphics.FillRectangle($dark,174,88,56,82)
        $graphics.FillRectangle($purple,174,150,56,20)
        $graphics.FillRectangle($teal,66,186,32,14)
        $graphics.FillRectangle($teal,40,200,84,12)
        $graphics.FillRectangle($purple,188,186,28,14)
        $graphics.FillRectangle($purple,170,200,64,12)
        $graphics.FillRectangle($light,180,156,8,8)
        $stream=[IO.MemoryStream]::new()
        $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
        $frames+=,@{Size=$size;Bytes=$stream.ToArray()}
        $stream.Dispose()
        if($size -eq 256){$bitmap.Save((Join-Path $PSScriptRoot 'oled-taskbar.png'),[Drawing.Imaging.ImageFormat]::Png)}
    } finally {$graphics.Dispose();$bitmap.Dispose();$dark.Dispose();$teal.Dispose();$purple.Dispose();$light.Dispose()}
}
$output=[IO.File]::Create((Join-Path $PSScriptRoot 'oled-taskbar.ico'))
$writer=[IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$frames.Count)
    $offset=6+16*$frames.Count
    foreach($frame in $frames){
        $dimension=if($frame.Size -eq 256){0}else{$frame.Size}
        $writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0)
        $writer.Write([uint16]1);$writer.Write([uint16]32);$writer.Write([uint32]$frame.Bytes.Length);$writer.Write([uint32]$offset)
        $offset+=$frame.Bytes.Length
    }
    foreach($frame in $frames){$writer.Write([byte[]]$frame.Bytes)}
} finally {$writer.Dispose();$output.Dispose()}
