Clevo Dual Fan Control Indicator for Ubuntu
===========================================

Fork of [clevo-indicator](https://github.com/SkyLandTW/clevo-indicator) by
AqD, modified to control **BOTH fans** (CPU + GPU) on dual-fan Clevo laptops.

The original `clevo-indicator` only ever issues the EC command `0x99 0x01`
which spins up the **CPU fan only**. On dual-fan models (P7xxDM, N1xxED,
NH5x_7x, ND, NE series, Schenker/XMG/Tuxedo rebrands, etc.) the second fan
(GPU) never spins up under manual/auto control — see upstream
[issue #27](https://github.com/SkyLandTW/clevo-indicator/issues/27).

This fork additionally issues `0x99 0x02` (the GPU-fan index) so **both fans**
respond. It also reads the GPU fan RPM from EC registers `0xD2/0xD3` so you
can actually confirm the second fan is turning.

It shows the CPU temperature and the GPU temperature in the tray, plus the
RPM of each fan in the tooltip, with a menu for manual control of both fans.

Why the second fan never spun up before
---------------------------------------
Upstream `ec_write_fan_duty()` is hard-coded to one EC sub-command index:

```c
return ec_io_do(0x99, 0x01, v_i);   // index 0x01 = CPU fan only
```

The Clevo EC fan-duty protocol uses the second byte as a **fan selector**:

| Index | Fan        |
|-------|------------|
| 0x01  | CPU fan    |
| 0x02  | GPU fan    |
| 0x03  | Fan #3 (if present) |
| 0xff  | switch fan to AUTO (followed by fan index) |

This fork writes both `0x01` and `0x02` for every duty change. Protocol
cross-checked against the official TUXEDO `tuxedo-fan-control` native helper
(`native/ec_access.cc`).

Build and Install
-----------------

```shell
sudo apt-get install libappindicator3-dev libgtk-3-dev
cd clevo-indicator-dual
make
sudo make install    # installs /usr/local/bin/clevo-indicator-dual with setuid
```

Or, to build in place without installing:

```shell
make
sudo chown root bin/clevo-indicator-dual
sudo chmod u+s   bin/clevo-indicator-dual
```

Command-line usage
------------------

```shell
# Show tray indicator (dual fan auto-control), needs DISPLAY set
clevo-indicator-dual

# Dump current fan state (temps + both fan RPMs), no GUI
clevo-indicator-dual -?

# Set BOTH fans to 80%
clevo-indicator-dual 80

# Set ONLY the CPU fan to 70%
clevo-indicator-dual 70 1

# Set ONLY the GPU fan to 70%
clevo-indicator-dual 70 2
```

Valid duty range is **40–100** (percent). Below ~40% most Clevo fans stall.

Notes
-----
- Same setuid=root trick as upstream: the binary runs with effective uid root
  (to access EC IO ports) but real uid = desktop user (to show the indicator).
  You'll see two processes in `ps`; killing either one kills the other.
- Do **not** run any other program that pokes the EC IO ports (`inb`/`outb`)
  at the same time — the EC needs ordered multi-byte command sequences and
  there is no kernel-level locking. This is the same caveat as upstream.
- Killing with `kill -9` is discouraged (use the Quit menu or a regular
  SIGTERM). The program catches normal termination signals to avoid leaving
  the EC mid-command.
- If the GPU fan RPM reads `0` but the CPU fan RPM reads fine, your model may
  use a different register for the second fan, or may only have one physical
  fan header. The write to index `0x02` is still safe on single-fan models
  (the EC simply ignores it).

Troubleshooting
---------------
- **`unable to control EC: Operation not permitted`** → the binary lacks
  setuid root. Re-run `sudo chmod u+s` / `sudo make install`.
- **Indicator doesn't appear** → you're running as root or with no DISPLAY.
  Run it as your normal desktop user (it escalates privileges itself).
- **GPU fan still 0 RPM after 100%** → some models report GPU RPM at
  `0xD4/0xD5` (Fan #3) instead of `0xD2/0xD3`. You can dump the full EC map
  with `sudo modprobe ec_sys && sudo od -Ax -t x1 /sys/kernel/debug/ec/ec0/io`
  and look for changing bytes while the fan spins.

License
-------
Same as upstream (see `LICENSE` upstream / GPL-compatible per the original
fanctrl.c by Jonas Diemer on which clevo-indicator is based).
