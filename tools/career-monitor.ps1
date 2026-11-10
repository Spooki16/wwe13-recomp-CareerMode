# WWE 13 career mode - live event monitor (Phase 0 test app).
#
# Connects to the game's career pipe (\\.\pipe\wwe13-career) and prints each event as it arrives. Start it before or
# after the game; it waits for the game and reconnects if the game restarts. Ctrl+C quits.
#
# From Ubuntu (WSL):   powershell.exe -ExecutionPolicy Bypass -File "$(wslpath -w tools/career-monitor.ps1)"
# From Windows:        powershell -ExecutionPolicy Bypass -File career-monitor.ps1
#
# -Raw prints the JSON lines exactly as the game sends them.

param(
  [string]$PipeName = "wwe13-career",
  [switch]$Raw
)

function Format-Seconds([double]$Seconds) {
  $span = [TimeSpan]::FromSeconds($Seconds)
  if ($span.TotalHours -ge 1) { return $span.ToString("h\:mm\:ss") }
  return $span.ToString("m\:ss")
}

function Show-Event($e, [string]$line) {
  $time = (Get-Date).ToString("HH:mm:ss")
  switch ($e.event) {
    "bridge_start"     { Write-Host "$time  Game started (bridge $($e.version))" -ForegroundColor Green }
    "entrance_start"   { Write-Host "$time  Entrance $($e.index) started" -ForegroundColor Cyan }
    "entrance_end"     { Write-Host "$time  Entrance $($e.index) ended  ($($e.duration) s)" -ForegroundColor Cyan }
    "match_start"      { Write-Host "$time  MATCH STARTED  (bell was $($e.after) s ago)" -ForegroundColor Yellow }
    "match_end"        {
      $pauses = if ($e.pauses) { "  (plus $(Format-Seconds $e.paused) paused, $($e.pauses) pause(s))" } else { "" }
      Write-Host "$time  MATCH OVER  after $(Format-Seconds $e.match)$pauses" -ForegroundColor Yellow
    }
    "match_summary"    { Write-Host "$time  Summary: $($e.entrances) entrance cinematic(s), $($e.entrance_time) s of entrances, match $(Format-Seconds $e.match)" -ForegroundColor Yellow }
    "pause_start"      { Write-Host "$time  Paused ($($e.phase))" -ForegroundColor DarkYellow }
    "pause_end"        { Write-Host "$time  Resumed after $(Format-Seconds $e.duration)" -ForegroundColor DarkYellow }
    "post_match_start" { Write-Host "$time  Post-match scene started" -ForegroundColor Magenta }
    "post_match_end"   { Write-Host "$time  Post-match scene ended  ($($e.duration) s)" -ForegroundColor Magenta }
    "cinematic_start"  { }
    "cinematic_end"    { }
    "scan_reset"       { Write-Host "$time  Scan: started over" -ForegroundColor White }
    "scan_mark"        { Write-Host "$time  Scan: marked $($e.state)  (A x$($e.marks_a), B x$($e.marks_b))  candidates: $($e.candidates)  [$($e.scan_seconds) s]" -ForegroundColor White }
    "scan_list"        {
      if ($e.file) {
        Write-Host "$time  Scan: $($e.candidates) candidate(s): $($e.flag_like) flag-like; $($e.image) in game globals, $($e.heap) in heaps, $($e.physical) in GPU memory" -ForegroundColor White
        Write-Host "            full list saved to logs\$($e.file); best candidates:" -ForegroundColor White
      } else {
        Write-Host "$time  Scan: $($e.candidates) candidate(s) $($e.note)" -ForegroundColor White
      }
    }
    "scan_candidate"   {
      $tag = if ($e.flag_like) { "flag-like" } else { "" }
      Write-Host ("            {0}   A={1}   B={2}   {3,-8} {4}" -f $e.address, $e.a, $e.b, $e.range, $tag) -ForegroundColor White
    }
    "watch"            {
      if ($e.first) { Write-Host "$time  Watch $($e.watch) = $($e.value)   (at $($e.address))" -ForegroundColor Green }
      else { Write-Host "$time  Watch $($e.watch): $($e.previous) -> $($e.value)" -ForegroundColor Green }
    }
    "watch_noisy"      { Write-Host "$time  Watch $($e.watch) is changing constantly (muted for a second)" -ForegroundColor DarkGreen }
    "watch_added"      { Write-Host "$time  Now watching $($e.watch)" -ForegroundColor Green }
    "scan_pointers"    { Write-Host "            pointer paths from the game's globals: $($e.count)" -ForegroundColor White }
    "scan_pointer"     { Write-Host "            $($e.candidate)  <-  $($e.path)" -ForegroundColor White }
    "scan_error"       { Write-Host "$time  Scan error: $($e.reason)" -ForegroundColor Red }
    default            { Write-Host "$time  $line" -ForegroundColor DarkGray }
  }
}

Write-Host "WWE 13 career monitor - waiting for the game (pipe \\.\pipe\$PipeName). Ctrl+C to quit."
while ($true) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", $PipeName, [System.IO.Pipes.PipeDirection]::In)
  try {
    $pipe.Connect(1000)
  } catch {
    $pipe.Dispose()
    continue
  }
  Write-Host "Connected to the game." -ForegroundColor Green
  $reader = New-Object System.IO.StreamReader($pipe)
  try {
    while ($null -ne ($line = $reader.ReadLine())) {
      if ($line.Length -eq 0) { continue }
      if ($Raw) { Write-Host $line; continue }
      try { $e = $line | ConvertFrom-Json } catch { Write-Host $line; continue }
      Show-Event $e $line
    }
  } catch {
  } finally {
    $reader.Dispose()
    $pipe.Dispose()
  }
  Write-Host "Game disconnected - waiting for it to start again." -ForegroundColor DarkYellow
}
