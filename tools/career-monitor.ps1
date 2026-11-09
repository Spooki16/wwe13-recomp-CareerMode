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
    "match_end"        { Write-Host "$time  MATCH OVER  after $(Format-Seconds $e.match)" -ForegroundColor Yellow }
    "match_summary"    { Write-Host "$time  Summary: $($e.entrances) entrance cinematic(s), $($e.entrance_time) s of entrances, match $(Format-Seconds $e.match)" -ForegroundColor Yellow }
    "post_match_start" { Write-Host "$time  Post-match scene started" -ForegroundColor Magenta }
    "post_match_end"   { Write-Host "$time  Post-match scene ended  ($($e.duration) s)" -ForegroundColor Magenta }
    "cinematic_start"  { }
    "cinematic_end"    { }
    "scan_reset"       { Write-Host "$time  Scan: started over" -ForegroundColor White }
    "scan_mark"        { Write-Host "$time  Scan: marked $($e.state)  (A x$($e.marks_a), B x$($e.marks_b))  candidates: $($e.candidates)  [$($e.scan_seconds) s]" -ForegroundColor White }
    "scan_list"        { Write-Host "$time  Scan: $($e.candidates) candidate(s) $($e.note)" -ForegroundColor White }
    "scan_candidate"   { Write-Host "            $($e.address)   A=$($e.a)   B=$($e.b)" -ForegroundColor White }
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
