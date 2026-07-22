Clevo / Axioo Dual Fan Control Indicator for Linux
==================================================

> Linux fan-control indicator for **Clevo-based laptops** and their Indonesian
> rebrands (e.g. **Axioo Pongo 725**). Fixes the common problem where only the
> **CPU fan spins** and the **GPU fan stays off**.

Fork of [clevo-indicator](https://github.com/SkyLandTW/clevo-indicator) by
AqD, modified to control **BOTH fans** (CPU + GPU) on dual-fan Clevo laptops.

Tested on **Axioo Pongo 725** (Clevo NH5xR barebone — Intel i7-12650H +
NVIDIA RTX 2050). Both fans confirmed spinning (CPU ~6200 RPM, GPU ~5600 RPM).

Tested on Arch-based: **CachyOS Linux** (KDE Plasma 6 Wayland, kernel 7.x)
— both fans read correctly via EC, icon visible in system tray.

The original `clevo-indicator` only ever issues the EC command `0x99 0x01`
which spins up the **CPU fan only**. On dual-fan models (P7xxDM, N1xxED,
NH5x_7x, ND, NE series, Axioo Pongo, Schenker/XMG/Tuxedo rebrands, etc.) the
second fan (GPU) never spins up under manual/auto control — see upstream
[issue #27](https://github.com/SkyLandTW/clevo-indicator/issues/27).

---

## Daftar Isi (untuk pengguna Indonesia)
- [Kenapa fan GPU saya tidak nyala?](#kenapa-fan-gpu-tidak-nyala)
- [Cara install di Debian/Ubuntu](#build-and-install) ← termasuk symlink ayatana-appindicator untuk distro baru
- [Cara install di Arch / CachyOS / Manjaro](#install-pada-arch-based-cachyos-manjaro-arch)
- [Cara pakai](#command-line-usage)
- [Bikin persistent (jalan terus / autostart)](#autostart-dengan-systemd)
- [Troubleshooting](#troubleshooting)

## Kenapa fan GPU tidak nyala?
Banyak laptop Clevo (termasuk Axioo Pongo, Schenker, XMG, Tuxedo) punya algoritma
bawaan EC yang rusak: fan GPU baru nyala kalau GPU sudah sangat panas. Karena GPU
modern (RTX 20/30/40) jarang panas, fan GPU jadi idle terus → laptop overheating
tanpa Anda sadari. Tool ini memaksa **kedua fan** ikut perintah yang sama.

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

### 1. Install dependencies

Pada distro lama (Ubuntu ≤ 20.04, Mint ≤ 20):
```shell
sudo apt-get install libappindicator3-dev libgtk-3-dev
```

Pada **distro terbaru** (Ubuntu 22.04+, Pop!_OS 22.04+, Debian 12, COSMIC),
`libappindicator` sudah diganti dengan **`ayatana-appindicator`**. Install paket
ini sebagai gantinya:
```shell
sudo apt-get install libayatana-appindicator3-dev libgtk-3-dev pkg-config
```

Karena source code original (warisan dari upstream) masih `#include` header
`libappindicator/app-indicator.h` dan Makefile memanggil
`pkg-config appindicator3-0.1`, Anda perlu **membuat symlink kompatibilitas**
supaya bisa compile tanpa ubah source:

```shell
# 1. Cek pkg-config ayatana ada (harusnya keluar path .pc-nya)
find / -name "ayatana-appindicator3*.pc" 2>/dev/null
#    -> /usr/lib/x86_64-linux-gnu/pkgconfig/ayatana-appindicator3-0.1.pc

# 2. Cek header app-indicator.h
find / -name "app-indicator.h" 2>/dev/null
#    -> /usr/include/libayatana-appindicator3-0.1/libayatana-appindicator/app-indicator.h

# 3. Buat symlink pkg-config (supaya `pkg-config appindicator3-0.1` jalan)
sudo ln -sf /usr/lib/x86_64-linux-gnu/pkgconfig/ayatana-appindicator3-0.1.pc \
            /usr/lib/x86_64-linux-gnu/pkgconfig/appindicator3-0.1.pc

# 4. Buat symlink header (supaya #include <libappindicator/app-indicator.h> lulus)
sudo mkdir -p /usr/include/libappindicator
sudo ln -sf /usr/include/libayatana-appindicator3-0.1/libayatana-appindicator/app-indicator.h \
            /usr/include/libappindicator/app-indicator.h
```

Verifikasi symlink sudah benar:
```shell
pkg-config --cflags --libs appindicator3-0.1
#    harusnya keluar flag -I... dan -layatana-appindicator3 ...
```

> **Catatan:** symlink ini aman — ayatana-appindicator adalah fork yang
> API-compatible dengan libappindicator, jadi binary hasil compile akan link ke
> libayatana tapi `#include` dan nama fungsi sama persis.

### 2. Build & install

```shell
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

Install pada Arch-based (CachyOS / Manjaro / Arch)
--------------------------------------------------

Distribusi Arch-family mengirim `libayatana-appindicator` (bukan legacy
`libappindicator`) dan tidak punya paket `brasero` default-nya. Repo ini
menyediakan **PKGBUILD siap-pakai** yang:

* menarik source via tag git `v1.0.0`
* otomatis apply `arch-build-fix.patch` (ayatana API macro + Breeze
  icons — lihat header patch untuk detail). File patch berada di root
  repo, bukan di `patches/`, karena AUR pre-receive hook menolak
  subdirektori.
* install binary ke `/usr/bin/clevo-indicator-dual` (Arch convention,
  BUKAN `/usr/local/bin` seperti Makefile upstream) dengan setuid root
* group ownership `wheel` (admin group default Arch/CachyOS)

### Install via AUR helper (`paru` / `yay`)

```bash
# setelah paket tersedia di AUR
paru -S clevo-indicator-dual
```

### Install manual dari repo ini

```bash
git clone https://github.com/hajilok/clevo-axioo-dual-fan-linux.git
cd clevo-axioo-dual-fan-linux
git checkout v1.0.0
makepkg -si    # -s sync deps, -i install setelah build sukses
```

`makepkg` otomatis menarik `base-devel`, `git`, `pkgconf`, `gtk3`,
`libayatana-appindicator` dari repo Arch. Patch ter-apply di phase
`prepare()`. Binary terinstall di `/usr/bin/clevo-indicator-dual`.

> **Catatan CachyOS:** user harus anggota group `wheel` agar bisa exec
> binary setuid. `wheel` adalah admin group default di Arch/CachyOS,
> paralel dengan `sudo`/`adm` di Debian/Ubuntu. Cek dengan `id $(whoami)`.

### Verifikasi

```bash
# binary harusnya: -rwsr-x--- root:wheel
ls -l /usr/bin/clevo-indicator-dual

# dump mode — harusnya bisa baca EC tanpa GUI
clevo-indicator-dual '-?'
```

Tested pada:
* **CachyOS Linux** (rolling, kernel 7.x, KDE Plasma 6 Wayland) — CPU
  fan terbaca ~2632 RPM, GPU fan ~2548 RPM; icon visible di system tray
  dengan status `NeedsAttention`.

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

Autostart dengan systemd
------------------------
Supaya indicator tetap muncul walau terminal di-close, dan auto-start saat
login, pakai systemd user service:

```bash
mkdir -p ~/.config/systemd/user

# Buat file ~/.config/systemd/user/clevo-indicator-dual.service
cat > ~/.config/systemd/user/clevo-indicator-dual.service <<'EOF'
[Unit]
Description=Clevo Dual Fan Control Indicator
After=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/local/bin/clevo-indicator-dual
Restart=on-failure
RestartSec=3
Environment=DISPLAY=:1
Environment=WAYLAND_DISPLAY=wayland-1
Environment=XDG_RUNTIME_DIR=/run/user/%U
Environment=GDK_BACKEND=wayland,x11
StartLimitIntervalSec=60
StartLimitBurst=5

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now clevo-indicator-dual.service
```

> **Catatan COSMIC/Wayland:** angka `DISPLAY=:1` dan `WAYLAND_DISPLAY=wayland-1`
> bisa beda tiap sesi. Cek dengan `echo $DISPLAY $WAYLAND_DISPLAY` lalu sesuaikan
> baris `Environment=` jika perlu.

Kontrol service:
```bash
systemctl --user status  clevo-indicator-dual   # cek status
systemctl --user stop    clevo-indicator-dual   # stop sementara
systemctl --user start   clevo-indicator-dual   # start lagi
systemctl --user disable clevo-indicator-dual   # matikan autostart
journalctl --user -u clevo-indicator-dual -f    # lihat log fan duty auto
```

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
