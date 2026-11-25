# Now-playing helper for iPodStats. Runs in Windows PowerShell 5.1 (needs WinRT access).
# Every second checks what's playing and writes nowplaying.json (+ art.jpg on track change) next to this
# script when it changed. Rests while iPodStats.exe reports that no screen is watching (idle.flag).
# iPodStats.exe reads those files and sends them to the iPod.

$mutex = New-Object System.Threading.Mutex($false, 'Local\iPodStatsMedia')
if (-not $mutex.WaitOne(0)) { exit } # already running

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
# If anything ever stops this helper, leave a note why (iPodStats.exe starts it again within ~10 s).
trap {
    try { Add-Content (Join-Path $dir 'media-error.log') ("{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), ($_ | Out-String)) } catch { }
    break
}
$jsonPath = Join-Path $dir 'nowplaying.json'
$artPath = Join-Path $dir 'art.jpg'
$accent = ''
$utf8 = New-Object System.Text.UTF8Encoding $false
$md5 = [System.Security.Cryptography.MD5]::Create()

Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Drawing
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
$asTaskFor = @{}
function Await($op, $type) {
    if (-not $asTaskFor.ContainsKey($type)) { $asTaskFor[$type] = $asTask.MakeGenericMethod($type) }
    $t = $asTaskFor[$type].Invoke($null, @($op))
    $t.Wait(3000) | Out-Null
    $t.Result
}

[void][Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
[void][Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType = WindowsRuntime]
$mgr = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

# Timeline catcher. Firefox shows a song's real length for only ~60 ms when it starts, then blanks it
# (measured 2026-11-14), so polling once a second misses it. Like Flylet, listen to the change event and
# copy the timeline at that very moment; the loop below replays the copies in order.
# Plain reflection, so it compiles without WinRT references in Windows PowerShell 5.1.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Reflection;
public static class TlCatcher {
    public class Snap { public string App; public double Pos, Start, End; public DateTimeOffset Upd, At; public bool Playing; }
    static readonly object gate = new object();
    static readonly List<Snap> queue = new List<Snap>();
    static readonly List<object> watched = new List<object>(); // sessions and handlers, kept alive
    public static string LastError;
    // WinRT events can't be joined with EventInfo.AddEventHandler; WindowsRuntimeMarshal.AddEventHandler is the supported way.
    static void Subscribe(object target, string eventName, string method) {
        EventInfo ev = target.GetType().GetEvent(eventName);
        Type h = ev.EventHandlerType;
        Type tok = typeof(System.Runtime.InteropServices.WindowsRuntime.EventRegistrationToken);
        Delegate handler = Delegate.CreateDelegate(h, typeof(TlCatcher).GetMethod(method, BindingFlags.NonPublic | BindingFlags.Static));
        Delegate add = Delegate.CreateDelegate(typeof(Func<,>).MakeGenericType(h, tok), target, ev.GetAddMethod());
        Delegate remove = Delegate.CreateDelegate(typeof(Action<>).MakeGenericType(tok), target, ev.GetRemoveMethod());
        MethodInfo addMethod = null;
        foreach (MethodInfo mi in typeof(System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeMarshal).GetMethods())
            if (mi.Name == "AddEventHandler" && mi.IsGenericMethodDefinition) addMethod = mi;
        addMethod.MakeGenericMethod(h).Invoke(null, new object[] { add, remove, handler });
        lock (gate) { watched.Add(handler); }
    }
    public static void Watch(object session) {
        if (session == null) return;
        lock (gate) {
            foreach (object o in watched) if (ReferenceEquals(o, session)) return;
            watched.Add(session); // try each session once
        }
        try { Subscribe(session, "TimelinePropertiesChanged", "OnChanged"); } catch (Exception e) { LastError = e.ToString(); }
    }
    // A reopened app is a new session: join it the moment Windows lists it, before its song starts.
    public static void WatchManager(object mgr) {
        try { Subscribe(mgr, "SessionsChanged", "OnSessions"); } catch (Exception e) { LastError = e.ToString(); }
        OnSessions(mgr, null);
    }
    static void OnSessions(object mgr, object args) {
        try {
            object list = mgr.GetType().GetMethod("GetSessions").Invoke(mgr, null);
            foreach (object s in (System.Collections.IEnumerable)list) Watch(s);
        } catch (Exception e) { LastError = e.ToString(); }
    }
    static object Get(object o, string name) { return o.GetType().GetProperty(name).GetValue(o, null); }
    static void OnChanged(object sender, object args) {
        try {
            object t = sender.GetType().GetMethod("GetTimelineProperties").Invoke(sender, null);
            object pb = sender.GetType().GetMethod("GetPlaybackInfo").Invoke(sender, null);
            Snap s = new Snap();
            s.At = DateTimeOffset.Now;
            s.App = (string)Get(sender, "SourceAppUserModelId");
            s.Pos = ((TimeSpan)Get(t, "Position")).TotalSeconds;
            s.Start = ((TimeSpan)Get(t, "StartTime")).TotalSeconds;
            s.End = ((TimeSpan)Get(t, "EndTime")).TotalSeconds;
            s.Upd = (DateTimeOffset)Get(t, "LastUpdatedTime");
            s.Playing = Get(pb, "PlaybackStatus").ToString() == "Playing";
            lock (gate) { queue.Add(s); if (queue.Count > 200) queue.RemoveAt(0); }
        } catch (Exception e) { LastError = e.ToString(); }
    }
    public static Snap[] Take() { lock (gate) { Snap[] a = queue.ToArray(); queue.Clear(); return a; } }
}
'@
[TlCatcher]::WatchManager($mgr)
$recentTl = New-Object System.Collections.ArrayList # caught timelines from the last few seconds

# Shrink cover art to a 240 px JPEG so the old iPod loads it fast.
# Raw bytes of the session's thumbnail, or $null if it has none.
function Get-ThumbBytes($thumb) {
    if (-not $thumb) { return $null }
    $ras = Await ($thumb.OpenReadAsync()) ([Windows.Storage.Streams.IRandomAccessStreamWithContentType])
    # Calling through reflection makes .NET cast the COM object to IInputStream for us.
    $m = [System.IO.WindowsRuntimeStreamExtensions].GetMethod('AsStreamForRead', [type[]]@([Windows.Storage.Streams.IInputStream]))
    $winStream = $m.Invoke($null, @($ras))
    $ms = New-Object System.IO.MemoryStream
    $winStream.CopyTo($ms)
    $winStream.Dispose()
    return , $ms.ToArray()
}

function Save-Art($bytes) {
    $src = New-Object System.IO.MemoryStream (, $bytes)
    $img = [System.Drawing.Image]::FromStream($src)
    $bmp = New-Object System.Drawing.Bitmap 240, 240
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    # Center-crop to a square (browser/YouTube thumbnails are often 16:9).
    $side = [Math]::Min($img.Width, $img.Height)
    $crop = New-Object System.Drawing.Rectangle ([int](($img.Width - $side) / 2)), ([int](($img.Height - $side) / 2)), $side, $side
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle 0, 0, 240, 240), $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $tmp = "$artPath.tmp"
    $bmp.Save($tmp, [System.Drawing.Imaging.ImageFormat]::Jpeg)
    Move-Item $tmp $artPath -Force

    # Accent color from the art (like Flylet's play button): average of the colorful pixels, lifted if too dark.
    $tiny = New-Object System.Drawing.Bitmap $bmp, 16, 16
    $r = 0.0; $gr = 0.0; $b = 0.0; $w = 0.0
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $c = $tiny.GetPixel($x, $y)
        $br = $c.GetBrightness()
        $wt = $c.GetSaturation() * $(if ($br -gt 0.2 -and $br -lt 0.85) { 1.0 } else { 0.2 })
        $r += $c.R * $wt; $gr += $c.G * $wt; $b += $c.B * $wt; $w += $wt
    } }
    $tiny.Dispose()
    if ($w -gt 4) {
        $r /= $w; $gr /= $w; $b /= $w
        $max = [Math]::Max($r, [Math]::Max($gr, $b))
        if ($max -lt 200 -and $max -gt 0) { $k = 200 / $max; $r *= $k; $gr *= $k; $b *= $k }
        $script:accent = '#{0:x2}{1:x2}{2:x2}' -f [int]$r, [int]$gr, [int]$b
    } else { $script:accent = '' }

    $g.Dispose(); $bmp.Dispose(); $img.Dispose(); $src.Dispose()
    return $true
}

# Friendly app name ("Firefox", "Spotify") from the session's app id, via the Start menu list.
$appNames = @{}
function Get-AppName($id) {
    if (-not $id) { return '' }
    if ($appNames.ContainsKey($id)) { return $appNames[$id] }
    $name = ''
    try { $name = [string](Get-StartApps | Where-Object AppID -eq $id | Select-Object -First 1).Name } catch { }
    if (-not $name) { $name = ($id -replace '\.exe$', '') -replace '^.*[\\!]', '' }
    $appNames[$id] = $name
    return $name
}

# Wallpapers: any picture dropped in backgrounds\ gets a 480x300 copy in backgrounds\cache\
# (the iPod has 256 MB of RAM, a full-size wallpaper would choke it). Removed pictures lose their copy.
$wallDir = Join-Path $dir 'backgrounds'
$wallCache = Join-Path $wallDir 'cache'
# Save a copy of a picture scaled to fill w x h (overflow cropped evenly). For GIFs this is the first frame.
function Save-Cover($srcPath, $outPath, $w, $h) {
    $img = [System.Drawing.Image]::FromFile($srcPath)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $scale = [Math]::Max($w / $img.Width, $h / $img.Height)
    $sw = [int][Math]::Ceiling($img.Width * $scale); $sh = [int][Math]::Ceiling($img.Height * $scale)
    $g.DrawImage($img, [int](($w - $sw) / 2), [int](($h - $sh) / 2), $sw, $sh)
    $g.Dispose(); $img.Dispose()
    $bmp.Save("$outPath.tmp", [System.Drawing.Imaging.ImageFormat]::Jpeg)
    $bmp.Dispose()
    Move-Item "$outPath.tmp" $outPath -Force
}

# Cheap fingerprint of the folder (names, sizes, dates). The real scan only runs when it changes.
$wallSig = ''
function Get-WallSig {
    $d = New-Object System.IO.DirectoryInfo $wallDir
    if (-not $d.Exists) { return '' }
    ($d.GetFiles() | ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join "`n"
}

# Is the cached copy at $path present and newer than the source picture $f?
function Fresh($path, $f) {
    $i = New-Object System.IO.FileInfo $path
    return $i.Exists -and $i.LastWriteTimeUtc -ge $f.LastWriteTimeUtc
}

function Update-Walls {
    $sig = Get-WallSig
    if ($sig -eq $script:wallSig -and (Test-Path $wallCache)) { return }
    $script:wallSig = $sig
    if (-not (Test-Path $wallDir)) { return }
    $thumbDir = Join-Path $wallCache 'thumbs'
    if (-not (Test-Path $thumbDir)) { New-Item -ItemType Directory $thumbDir -Force | Out-Null }
    $sources = @(Get-ChildItem $wallDir -File | Where-Object { $_.Extension -match '^\.(jpe?g|png|bmp|gif)$' })
    $want = @{}
    foreach ($f in $sources) {
        # Cache names are plain ASCII (the server can't open names with special characters) + a short hash to stay unique.
        $hash = [BitConverter]::ToString($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($f.Name))).Replace('-', '').Substring(0, 6)
        $safe = ($f.BaseName -replace '[^A-Za-z0-9_-]', '_') + '-' + $hash
        try {
            # Small preview for the picker on the iPod (same name as the cached picture, always .jpg).
            $thumb = Join-Path $thumbDir ($safe + '.jpg')
            $want[$thumb] = $true
            if (-not (Fresh $thumb $f)) { Save-Cover $f.FullName $thumb 110 69 }

            # Animated GIFs are copied as they are so they keep moving (the iPod scales them). Limit 8 MB.
            $gifOut = Join-Path $wallCache ($safe + '.gif')
            $jpgOut = Join-Path $wallCache ($safe + '.jpg')
            if (Fresh $gifOut $f) { $want[$gifOut] = $true; continue } # animated copy already made
            if ($f.Extension -eq '.gif' -and $f.Length -le 8MB -and -not (Fresh $jpgOut $f)) {
                $probe = [System.Drawing.Image]::FromFile($f.FullName)
                $frames = $probe.GetFrameCount((New-Object System.Drawing.Imaging.FrameDimension $probe.FrameDimensionsList[0]))
                $probe.Dispose()
                if ($frames -gt 1) {
                    $out = $gifOut
                    $want[$out] = $true
                    Copy-Item -LiteralPath $f.FullName "$out.tmp" -Force
                    Move-Item "$out.tmp" $out -Force
                    continue
                }
            }
            # Everything else: a 480x300 copy (the iPod has 256 MB of RAM, a full-size wallpaper would choke it).
            $out = Join-Path $wallCache ($safe + '.jpg')
            $want[$out] = $true
            if (-not (Fresh $out $f)) { Save-Cover $f.FullName $out 480 300 }
        } catch { }
    }
    Get-ChildItem $wallCache, $thumbDir -File | Where-Object { -not $want.ContainsKey($_.FullName) } | Remove-Item -Force
    [GC]::Collect() # pictures were decoded: hand the memory back
}

# ---------- Song position (port of Flylet's TimelineController) ----------
# Sources rarely report the position while playing. Firefox resets the timeline to position 0 and
# length 0 shortly after a song starts or seeks, and only reports the real one again on pause.
# So: keep the last real timeline for this song and keep counting from it while playing.
$tl = @{ Has = $false; Start = 0.0; End = 0.0; BasePos = 0.0; BaseTime = [DateTimeOffset]::MinValue
         Playing = $false; LastPos = $null; LastUpd = $null }
function Clear-Timeline { $tl.Has = $false; $tl.Playing = $false; $tl.LastPos = $null; $tl.LastUpd = $null }
function Get-TimelinePos($now) {
    $pos = $tl.BasePos
    if ($tl.Playing -and $now -gt $tl.BaseTime) { $pos += ($now - $tl.BaseTime).TotalSeconds }
    if ($pos -lt $tl.Start) { return $tl.Start }
    if ($tl.End -gt $tl.Start -and $pos -gt $tl.End) { return $tl.End } # end <= start: unknown length (live)
    return $pos
}
# One report from the source: positions in seconds, $upd = when it measured the position.
function Update-Timeline($pos, $start, $end, $upd, $playing, $now) {
    $empty = $pos -eq 0 -and $end -le $start # what sources send when they have no timeline
    if ($empty -and $tl.Has) {
        # No new information: keep the last real timeline, only take the new play state
        $tl.BasePos = Get-TimelinePos $now; $tl.BaseTime = $now
    } else {
        $fresh = -not $tl.Has -or $pos -ne $tl.LastPos -or $upd -ne $tl.LastUpd
        if ($fresh) {
            $tl.BasePos = $pos
            $tl.BaseTime = $(if ($upd.Year -gt 2000) { $upd } else { $now })
        } else {
            # The same position read again (e.g. on play/pause): carry on from the current estimate
            $tl.BasePos = Get-TimelinePos $now; $tl.BaseTime = $now
        }
        $tl.Has = -not $empty; $tl.Start = $start; $tl.End = $end
        $tl.LastPos = $pos; $tl.LastUpd = $upd
    }
    $tl.Playing = $playing
}

$lastKey = $null
$tlKey = $null
$artId = 0
$hasArt = $false
$sinceChange = 0
$lastHash = $null
$loop = 0
$idleFlag = Join-Path $dir 'idle.flag'
$lastJson = ''
$lastWrite = [DateTime]::MinValue
while ($true) {
    Start-Sleep -Milliseconds 1000
    # iPodStats.exe creates idle.flag while no screen is showing the page: do nothing but say "still here".
    if ([System.IO.File]::Exists($idleFlag)) {
        try {
            if ([DateTime]::UtcNow - $lastWrite -gt [TimeSpan]::FromSeconds(3) -and [System.IO.File]::Exists($jsonPath)) {
                [System.IO.File]::SetLastWriteTimeUtc($jsonPath, [DateTime]::UtcNow); $lastWrite = [DateTime]::UtcNow
            }
        } catch { }
        continue
    }
    if ($loop++ % 5 -eq 0) { try { Update-Walls } catch { } }
    try {
        $s = $mgr.GetCurrentSession()
        $data = @{ status = 'None'; title = ''; artist = ''; album = ''; pos = 0; dur = 0; art = 0; app = ''; accent = '' }
        if ($s) {
            $p = Await ($s.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
            $t = $s.GetTimelineProperties()
            $data.status = [string]$s.GetPlaybackInfo().PlaybackStatus
            $data.title = [string]$p.Title
            $data.artist = [string]$p.Artist
            $data.album = [string]$p.AlbumTitle
            $songKey = "$($p.Title)|$($p.Artist)"
            $now = [DateTimeOffset]::Now
            $newSong = $songKey -ne $tlKey
            if ($newSong) { $tlKey = $songKey; Clear-Timeline } # new song: forget the old timeline
            [TlCatcher]::Watch($s)
            if ([TlCatcher]::LastError) { # never stops the helper, but leaves a note
                try { Add-Content (Join-Path $dir 'media-error.log') ("{0}  timeline catcher: {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), [TlCatcher]::LastError) } catch { }
                [TlCatcher]::LastError = $null
            }
            # Timelines caught the moment they changed, in order. Firefox can send the new song's length a
            # moment before its new title, so on a new song replay the last 3 s, not only what is new.
            $caught = [TlCatcher]::Take()
            foreach ($c in $caught) { [void]$recentTl.Add($c) }
            while ($recentTl.Count -gt 0 -and ($now - $recentTl[0].At).TotalSeconds -gt 5) { $recentTl.RemoveAt(0) }
            $replay = $(if ($newSong) { @($recentTl | Where-Object { ($now - $_.At).TotalSeconds -le 3 }) } else { $caught })
            foreach ($c in $replay) {
                if ($c.App -ne $s.SourceAppUserModelId) { continue }
                Update-Timeline $c.Pos $c.Start $c.End $c.Upd $c.Playing $c.At
            }
            Update-Timeline $t.Position.TotalSeconds $t.StartTime.TotalSeconds $t.EndTime.TotalSeconds $t.LastUpdatedTime ($data.status -eq 'Playing') $now
            if ($tl.Has) {
                $data.dur = [int]($tl.End - $tl.Start)
                $data.pos = [int][Math]::Max(0, (Get-TimelinePos $now) - $tl.Start)
            }

            # Browsers show their own logo first and send the real cover a moment after the song starts,
            # so keep checking the picture: every second for 10 s after a song change, then every 5 s up to a minute.
            $key = "$($p.Title)|$($p.Artist)"
            if ($key -ne $lastKey) { $lastKey = $key; $sinceChange = 0 }
            $sinceChange++
            if ($sinceChange -le 10 -or ($sinceChange -le 60 -and $sinceChange % 5 -eq 0)) {
                try {
                    $bytes = Get-ThumbBytes $p.Thumbnail
                    if (-not $bytes) { $hasArt = $false; $lastHash = $null }
                    else {
                        $hash = [BitConverter]::ToString($md5.ComputeHash($bytes))
                        if ($hash -ne $lastHash) { $lastHash = $hash; $hasArt = Save-Art $bytes; $artId++ }
                    }
                } catch { }
            }
            if ($hasArt) { $data.art = $artId; $data.accent = $accent }
            $data.app = Get-AppName $s.SourceAppUserModelId
        }
        $json = $data | ConvertTo-Json -Compress
        if ($json -ne $lastJson) {
            $tmp = "$jsonPath.tmp"
            [System.IO.File]::WriteAllText($tmp, $json, $utf8)
            Move-Item $tmp $jsonPath -Force
            $lastJson = $json; $lastWrite = [DateTime]::UtcNow
        } elseif ([DateTime]::UtcNow - $lastWrite -gt [TimeSpan]::FromSeconds(3)) {
            # Paused / nothing playing: no rewrite, just show the server the helper is alive.
            [System.IO.File]::SetLastWriteTimeUtc($jsonPath, [DateTime]::UtcNow); $lastWrite = [DateTime]::UtcNow
        }
    } catch { }
}
