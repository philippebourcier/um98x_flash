# um98x_flash

Flash Unicore UM98x (UM980, UM982, …) firmware packages from Linux, over a serial port wired to the receiver's COM1 (USB-UART adapter or a board with a built-in one).

Single C file, no dependencies. The bootloader logic is a port of decipede's `um980_fw.c`.

## Build

```
gcc -O2 -Wall -o um98x_flash um98x_flash.c
```

## Usage

```
./um98x_flash [-d dev] [-b baud] [-f fastbaud|0] [-r cmd|rts|dtr|none] [-V version] [-y] [-v] [-j] firmware.pkg
```

| Option | Default | Description |
|---|---|---|
| `-d` | `/dev/ttyUSB0` | Serial port wired to **COM1** |
| `-b` | `115200` | Current COM1 baud rate (fallback transfer rate) |
| `-f` | `921600` | Fast transfer rate, `0` to disable |
| `-r` | `cmd` | How to restart the receiver into its bootloader (see below) |
| `-V` | from file name | Package version, e.g. `R4.10Build20739` |
| `-y` | | Force: flash despite a model mismatch or a build not newer than the receiver's |
| `-v` | | Echo what the receiver sends (stderr) |
| `-j` | | Machine-readable output (JSON Lines, see below) |

Examples:

```
./um98x_flash -d /dev/ttyUSB0 UM980_R4.10Build20739.pkg
./um98x_flash -d /dev/ttyACM0 -f 0 -r none UM980_R4.10Build20739.pkg   # rescue
```

## What it does

1. Checks the package: Unicore magic and product name (header offset `0x9C`).
2. Queries the receiver (`VERSIONA`): refuses a package for another model, skips if the receiver already runs the target build or newer (unless `-y`).
3. Switches COM1 to the fast rate if the receiver accepts it.
4. Restarts the receiver and spams the bootloader trigger until its menu shows up.
5. Sends the package (XMODEM-1K variant), waits for the receiver to verify it (up to 90 s), leaves the bootloader.
6. Queries the receiver again and checks it runs the target build.

If the transfer fails at the fast rate, it is retried once at the normal rate.

### Target version

Taken from `-V`, or else from a Unicore-style file name (`UM980_R4.10Build20739.pkg` → `R4.10Build20739`). Without either, the tool flashes unconditionally and doesn't check the result's version.

A file name is only a hint: a misnamed package is caught after flashing (`version not confirmed`), not before. Pass `-V` when the version comes from a more reliable source.

### Reset methods (`-r`)

There is no reset line on a PC, so:

- `cmd`: send `RESET` to the receiver (default).
- `rts` / `dtr`: 500 ms pulse on that line, for boards wiring it to RESET. Polarity depends on the adapter.
- `none`: wait up to 60 s for a manual power cycle.

If the receiver doesn't answer `VERSIONA` at start (e.g. stuck in its bootloader after a failed flash), the tool switches to rescue mode: normal rate, and `-r cmd` becomes `none`.

## Machine output (`-j`)

One JSON object per line on stdout, flushed after each line. Nothing else is written to stdout (`-v` goes to stderr).

```
{"event":"package","product":"UM980","size":3038408}
{"event":"log","msg":"version R4.10Build20739 taken from the file name"}
{"event":"receiver","model":"UM980","firmware":"R4.10Build17548"}
{"event":"stage","stage":"baud","msg":"switching to 921600 baud"}
{"event":"stage","stage":"bootloader","msg":"entering bootloader at 921600 baud"}
{"event":"stage","stage":"transfer","msg":"bootloader menu reached, sending 3038408 bytes"}
{"event":"progress","percent":42,"sent":1276928,"total":3038408}
{"event":"stage","stage":"verify","msg":"transfer complete, receiver verifying (up to 90 s)"}
{"event":"stage","stage":"reboot","msg":"leaving bootloader"}
{"event":"stage","stage":"check","msg":"waiting for the receiver to answer"}
{"event":"result","ok":true,"skipped":false,"firmware":"R4.10Build20739","previous":"R4.10Build17548","error":null}
```

| Event | Fields | Meaning |
|---|---|---|
| `package` | `product`, `size` | Package accepted |
| `receiver` | `model`, `firmware` | Receiver before flashing (absent in rescue mode) |
| `stage` | `stage`, `msg` | `baud`, `bootloader`, `transfer`, `verify`, `reboot`, `check`; `bootloader` onwards can repeat on a retry |
| `progress` | `percent`, `sent`, `total` | At most one per percent |
| `action` | `msg` | **A person must act** (power cycle): show it prominently |
| `log` | `msg` | Informational |
| `error` | `msg` | Detail of a failure; the `result` line follows |
| `result` | `ok`, `skipped`, `firmware`, `previous`, `error` | Always the last line; `skipped` is true when nothing needed flashing |

`result` is emitted on every outcome except argument errors, which print the usage on stderr and exit with 1.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Flashed and confirmed, or already up to date |
| `1` | Failed, receiver still answering (or bad arguments, package or port) |
| `2` | Receiver not answering afterwards: retry, in rescue mode if needed |

### Calling it from a web app

Run it as a subprocess, read stdout line by line and relay each line as is (SSE, WebSocket). Make sure only one flash runs per serial port at a time: the tool doesn't lock the tty. Don't kill it during the `transfer` or `verify` stages.

## Notes

- The bootloader only talks on **COM1**. On dual-port boards (e.g. CH342), use the port wired to COM1.
- If the USB-UART adapter doesn't do 921600, use `-f 0`.
- If the bootloader menu is never reached with `-r cmd`, use `-r none` and power-cycle the receiver by hand.
- `-v` shows the bootloader dialogue, the first thing to look at when something goes wrong.

## License

Copyright 2026 Philippe Bourcier

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
