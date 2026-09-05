# Raw control-channel client for RDR2Lua. The counterpart to deploy.ps1: that one
# pushes files over FTP, this one talks to the running plugin on TCP 9616.
#
#   pwsh tools/console.ps1 hello
#   pwsh tools/console.ps1 reslist
#   pwsh tools/console.ps1 resinfo hello
#   pwsh tools/console.ps1 start  my-resource
#   pwsh tools/console.ps1 cmd    "where"
#   pwsh tools/console.ps1 exec   "print(('z'):rep(5000))"
#   pwsh tools/console.ps1 execfile tools/native_crosscheck.lua
#   pwsh tools/console.ps1 log                      # just watch, 10 s
#   pwsh tools/console.ps1 exec "print(1)" -Bytes   # show reply/log LENGTHS
#   pwsh tools/console.ps1 reslist -Token secret    # console has /data/rdr2lua/token
#
# -Token is only needed when that file exists on the console. It is sent as the
# HELLO payload, and because a console with a token refuses everything else
# until that handshake passes, this tool does the handshake first on the same
# connection rather than sending its one frame and getting "unauthenticated".
#
# Why this exists rather than "use the VS Code extension": the extension only
# offers what its UI models. It builds its resource picker out of RESLIST, so
# there is no way to send a name RESLIST never returned -- which is exactly what
# verifying the wire's name limit needs. Every finding in
# docs/post-m2a-verification-session.md was measured through here.
#
# On connect the server flushes its outbox backlog, so the first command of a
# session prints whatever the ring still held -- up to 256 lines that predate
# you. Those are history, not a response to what you just sent.
#
# -Bytes is not a debug flag, it is the point. "the line looks long" is not a
# test; "the payload is 255 bytes, 218 of them 'z'" is. A truncation marker in
# particular cannot be checked by eye at the end of a 255-character line.
param(
    [Parameter(Position = 0)][ValidateSet('hello','ping','reslist','resinfo','start','stop','restart','cmd','exec','execfile','reload','log')]
    [string]$Command = 'hello',
    [Parameter(Position = 1)][string]$Arg = '',
    [string]$Ip = "10.10.10.235",
    [int]$Port = 9616,
    [int]$WaitMs = 5000,
    [string]$Token = "",
    [switch]$Bytes
)
$ErrorActionPreference = "Stop"
$script:failed = $false

# src/net/frame.h. Kept in sync by hand; tests/protocol_vectors.json is the
# binding copy for the C++ and TypeScript sides, and this file is a diagnostic
# tool rather than a fourth implementation to gate.
$OP = @{ HELLO=1; PING=2; PONG=3; PUT=4; GET=5; LS=6; RM=7; EXEC=8; RELOAD=9
         LOG=10; EVENT=11; OK=12; ERR=13; BUSY=14; START=15; STOP=16
         RESTART=17; RESLIST=18; RESINFO=19; CMD=20 }
$OPNAME = @{}; $OP.GetEnumerator() | ForEach-Object { $OPNAME[[int]$_.Value] = $_.Key }

function Read-Exact($s, [int]$n) {
    $buf = New-Object byte[] $n; $got = 0
    while ($got -lt $n) {
        $r = $s.Read($buf, $got, $n - $got)
        if ($r -le 0) { throw "connection closed after $got of $n bytes" }
        $got += $r
    }
    ,$buf
}

# magic 'GLUA' (4) | op (1) | flags (1) | seq (2 LE) | len (4 LE) | payload
function Read-Frame($s) {
    $h = Read-Exact $s 12
    if ([Text.Encoding]::ASCII.GetString($h, 0, 4) -ne 'GLUA') { throw "bad magic in header" }
    $len = [BitConverter]::ToUInt32($h, 8)
    $p = if ($len -gt 0) { Read-Exact $s $len } else { ,(New-Object byte[] 0) }
    [pscustomobject]@{
        Op   = [int]$h[4]; OpName = $OPNAME[[int]$h[4]]
        Seq  = [BitConverter]::ToUInt16($h, 6); Len = [int]$len
        Text = [Text.Encoding]::UTF8.GetString($p); Bytes = $p
    }
}

# $opcode, not $op: PowerShell variable names are CASE-INSENSITIVE, so an $op
# anywhere in this file is the same variable as the $OP opcode table. As a
# parameter here it shadows the table for the length of the function; as a
# script-level assignment it destroys it outright. See the note at the call
# site below -- this cost a debugging round, and silently.
function Send-Frame($s, [int]$opcode, [int]$seq, [byte[]]$payload) {
    $len = if ($payload) { $payload.Length } else { 0 }
    $h = New-Object byte[] 12
    [Text.Encoding]::ASCII.GetBytes('GLUA').CopyTo($h, 0)
    $h[4] = [byte]$opcode; $h[5] = 0
    [BitConverter]::GetBytes([uint16]$seq).CopyTo($h, 6)
    [BitConverter]::GetBytes([uint32]$len).CopyTo($h, 8)
    $s.Write($h, 0, 12)
    if ($len -gt 0) { $s.Write($payload, 0, $len) }
    $s.Flush()
}

$c = New-Object System.Net.Sockets.TcpClient
try { $c.Connect($Ip, $Port) }
catch { Write-Error "cannot reach ${Ip}:${Port} - is the game running with the plugin loaded?"; exit 1 }
$s = $c.GetStream()
$s.ReadTimeout = [Math]::Max(1000, $WaitMs)

# A console carrying /data/rdr2lua/token answers everything but HELLO and
# PING with "unauthenticated" (src/net/auth.h). This tool sends ONE frame
# per run and never used to say hello at all, so with a token file present
# every command would be refused -- including the ones the checklist uses to
# verify the token works. So authenticate first, on this same connection,
# and stop if that fails: the refusal is worth reading, and continuing would
# only produce a second, more confusing one.
#
# It sits BEFORE the try/finally below on purpose. Those two exits have to carry
# a code out to the caller, and this file already learned once that `exit` does
# not do that reliably from inside that block -- see the note at its end. So the
# socket is closed by hand on these paths instead.
$seq = 1
if ($Token -and $Command -ne 'hello') {
    Send-Frame $s $OP.HELLO $seq ([Text.Encoding]::UTF8.GetBytes($Token))
    $auth = $null; $authDeadline = (Get-Date).AddMilliseconds($WaitMs)
    while ((Get-Date) -lt $authDeadline) {
        try { $f = Read-Frame $s } catch { break }
        if ($f.Seq -eq $seq -and $f.Op -ne $OP.LOG -and $f.Op -ne $OP.EVENT) { $auth = $f; break }
        # The backlog the server flushes on connect now arrives HERE, ahead of
        # the handshake reply. Printed rather than swallowed: the header of this
        # file promises the first command of a session shows it, and -Token must
        # not quietly eat up to 256 lines of history.
        if ($Bytes) { "  [{0} {1,4}b] {2}" -f $f.OpName, $f.Len, $f.Text }
        else        { "  [$($f.OpName)] $($f.Text)" }
    }
    if (-not $auth) { Write-Error "no reply to the token HELLO within ${WaitMs} ms"; $c.Close(); exit 1 }
    # Anything that is not a HELLO reply means the handshake did not happen.
    # BUSY especially: the protocol models it explicitly, and treating it as
    # success would send the real command anyway and get back
    # 'unauthenticated' -- the second, more confusing error this block exists
    # to prevent.
    if ($auth.Op -ne $OP.HELLO) {
        Write-Error "handshake refused: $($auth.OpName) $($auth.Text)"; $c.Close(); exit 1
    }
    $seq = 2
}

try {
    if ($Command -eq 'log') {
        "watching LOG frames for 10 s (Ctrl+C to stop)"
        $end = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $end) {
            try { $f = Read-Frame $s } catch { break }
            if ($Bytes) { "[{0} {1,4}b] {2}" -f $f.OpName, $f.Len, $f.Text } else { $f.Text }
        }
        exit 0
    }

    # execfile is exec with the chunk read from disk. A generated audit like
    # tools/native_crosscheck.lua is a hundred kilobytes of table; passing that
    # as a command-line argument is not workable, and the wire takes it happily
    # (MAX_PAYLOAD is 8 MiB).
    if ($Command -eq 'execfile') {
        if (-not (Test-Path $Arg)) { Write-Error "no such file: $Arg"; exit 1 }
        $Arg = [IO.File]::ReadAllText($Arg)
        $Command = 'exec'
    }

    # NOT $op. `$op = $OP[...]` reads the table and then overwrites it with the
    # integer it just read, because the two names differ only in case. The
    # failure that follows is quiet rather than loud: $OP.LOG and $OP.EVENT in
    # the reply loop below silently become $null, so those two guards stop
    # guarding, and the loop keeps working only because the seq check already
    # excludes unsolicited frames. What actually surfaced was $OP.ERR going
    # empty, which cost this tool its non-zero exit code on a failed request.
    $opcode = $OP[$Command.ToUpper()]
    $payload = switch ($Command) {
        # "chunkname\0<lua source>" -- see on_exec in src/net/dispatch.cpp.
        'exec' { [byte[]]([Text.Encoding]::UTF8.GetBytes("console") + @([byte]0) + [Text.Encoding]::UTF8.GetBytes($Arg)) }
        # HELLO's payload IS the token, when there is one -- so `console.ps1
        # hello -Token x` is the way to test a token without sending anything
        # else, which is exactly what the checklist wants.
        'hello' { if ($Token) { [Text.Encoding]::UTF8.GetBytes($Token) } else { $null } }
        default { if ($Arg) { [Text.Encoding]::UTF8.GetBytes($Arg) } else { $null } }
    }
    Send-Frame $s $opcode $seq $payload

    # Unsolicited LOG/EVENT frames carry seq 0 and can arrive before the reply.
    # They are printed rather than dropped: a chunk's print() output is often
    # the thing being measured, and it never comes back in the reply.
    $reply = $null; $deadline = (Get-Date).AddMilliseconds($WaitMs)
    while ((Get-Date) -lt $deadline) {
        try { $f = Read-Frame $s } catch { break }
        if ($f.Seq -eq $seq -and $f.Op -ne $OP.LOG -and $f.Op -ne $OP.EVENT) { $reply = $f; break }
        if ($Bytes) { "  [{0} {1,4}b] {2}" -f $f.OpName, $f.Len, $f.Text } else { "  [$($f.OpName)] $($f.Text)" }
    }

    if (-not $reply) {
        # The server keeps ONE client (g_client in src/net/net_server.cpp). A
        # second connection is accepted by the kernel and then never serviced,
        # so this looks like a hang rather than a refusal -- and the usual cause
        # is the VS Code extension still being connected. Say so instead of
        # letting the caller wonder.
        Write-Error ("no reply within ${WaitMs} ms. The control channel serves one client at a time - " +
                     "if the VS Code extension is connected, run RDR2Lua: Disconnect first.")
        exit 1
    }

    if ($Bytes) { "{0} ({1} bytes)" -f $reply.OpName, $reply.Len }
    else        { $reply.OpName }
    $reply.Text

    # Anything the chunk PRINTED arrives after the reply, not in it: EXEC
    # answers with the chunk's return value while print() goes down the log
    # path. Stopping at the reply therefore hid exactly what this tool's own
    # documented example ("print(('z'):rep(5000))") exists to measure, which is
    # how this was found. Only for the commands that can run Lua -- a RESLIST
    # has nothing to follow and should not cost a second.
    if ($Command -in @('exec','cmd','start','stop','restart','reload')) {
        $end = (Get-Date).AddMilliseconds(1500)
        while ((Get-Date) -lt $end) {
            try { $f = Read-Frame $s } catch { break }
            if ($Bytes) { "  [{0} {1,4}b] {2}" -f $f.OpName, $f.Len, $f.Text }
            else        { "  [$($f.OpName)] $($f.Text)" }
        }
    }

    # exit inside try/finally does not carry the code out reliably here, so the
    # status is recorded and set once, at the end, outside the block.
    if ($reply.Op -eq $OP.ERR) { $script:failed = $true }
}
finally { $c.Close() }

if ($script:failed) { exit 2 }
