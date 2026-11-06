# dirsize

Fast parallel disk-usage report for Linux, written in C (pthreads).
Starts at `/` by default and shows where the space in `df -h` actually goes —
plus storage devices, NVMe temperature / wear / TBW, usage by file type and the largest files.

```
      ___                       ___           ___                       ___           ___
     /\  \          ___        /\  \         /\  \          ___        /\  \         /\  \
    /::\  \        /\  \      /::\  \       /::\  \        /\  \       \:\  \       /::\  \
   /:/\:\  \       \:\  \    /:/\:\  \     /:/\ \  \       \:\  \       \:\  \     /:/\:\  \
  /:/  \:\__\      /::\__\  /::\~\:\  \   _\:\~\ \  \      /::\__\       \:\  \   /::\~\:\  \
 /:/__/ \:|__|  __/:/\/__/ /:/\:\ \:\__\ /\ \:\ \ \__\  __/:/\/__/ _______\:\__\ /:/\:\ \:\__\
 \:\  \ /:/  / /\/:/  /    \/_|::\/:/  / \:\ \:\ \/__/ /\/:/  /    \::::::::/__/ \:\~\:\ \/__/
  \:\  /:/  /  \::/__/        |:|::/  /   \:\ \:\__\   \::/__/      \:\~~\~~      \:\ \:\__\
   \:\/:/  /    \:\__\        |:|\/__/     \:\/:/  /    \:\__\       \:\  \        \:\ \/__/
    \::/__/      \/__/        |:|  |        \::/  /      \/__/        \:\__\        \:\__\
     ~~                        \|__|         \/__/                     \/__/         \/__/
                                                            v2.2.0 · fast parallel disk usage

▌ Storage devices
  nvme0n1  SOLIDIGM SSDPFKKW020X7 · NVMe SSD · 1.9T
             Temp     50°C  ██████░░░░  warn 86°C  · sensors 44/44°C
             Wear     ? (run with sudo for wear % / TBW)  · since boot: 333.7 GB written, 2.3 TB read
  ├─ nvme0n1p1    vfat   /boot/efi        6.4M / 1.1G   █░░░░░░░░░░░░░░░░░░░   1%  free 1.1G
  └─ nvme0n1p2    ext4   /                1.3T / 1.9T   ██████████████░░░░░░  70%  free 544G

▌ / usage  (/dev/nvme0n1p2 → /)
  Size 1.9T   Used 1.3T   Avail 544G   Use% 70%
  ███████████████████████████████████░░░░░░░░░░░░░░░

  Scanned files/dirs      1.1T   85.9%  █████████████████░░░
  Not scanned             175G   14.1%  ███░░░░░░░░░░░░░░░░░  permission denied · FS metadata · deleted-but-open files
  Reserved                 96G          root-only reserved blocks, not in Used/Avail
  Inodes used             3.9M / 125.0M  (3.1%)
  0.33s · 306134 dirs · 2563059 files · 64 threads

▌ Directory tree (depth 2 · top 10 per dir)
   1.1T 100.0% ██████████  /  ! read errors
   755G  71.0% ███████░░░  ├── data/  ! read errors
   755G  71.0% ███████░░░  │   └── parksThink/  ! read errors
   175G  16.4% ██░░░░░░░░  ├── home/
   175G  16.4% ██░░░░░░░░  │   └── park2/
    66G   6.2% █░░░░░░░░░  ├── backup/
    66G   6.2% █░░░░░░░░░  │   └── parks-zbook/
   ...

▌ By file type
  AI models / weights           601G  56.6%  ███████████░░░░░░░░░     7163 files  .safetensors .gguf .pt .bin
  Other / no extension          149G  14.0%  ███░░░░░░░░░░░░░░░░░   705347 files  
  Libraries / binaries          115G  10.8%  ██░░░░░░░░░░░░░░░░░░   189879 files  .so .a .o .pyc
  Archives / disk images        112G  10.5%  ██░░░░░░░░░░░░░░░░░░    41822 files  .zip .tar.gz .iso .ipsw
  Datasets / DB                  32G   3.0%  █░░░░░░░░░░░░░░░░░░░   468469 files  .npy .parquet .csv .db
  Audio                          22G   2.0%  █░░░░░░░░░░░░░░░░░░░    36982 files  .mp3 .flac .wav
  Source code                    14G   1.2%  █░░░░░░░░░░░░░░░░░░░   735286 files  .c .py .js .rs
  Documents                     6.6G   0.6%  █░░░░░░░░░░░░░░░░░░░   111402 files  .pdf .docx .hwp .md
  Logs                          5.4G   0.5%  █░░░░░░░░░░░░░░░░░░░   104682 files  .log .journal
  Images                        4.0G   0.4%  █░░░░░░░░░░░░░░░░░░░    93710 files  .jpg .png .heic
  Video                         3.4G   0.3%  █░░░░░░░░░░░░░░░░░░░      524 files  .mp4 .mkv .mov

▌ Largest 5 files
      26G  /data/.../MiniMax-H3/text_encoders/qwen3vl_32b_minimax_h3_int8_convrot.safetensors
      20G  /data/.../MiniMax-H3/diffusion_models/minimax_h3_ref2va_pruned_int8_convrot.safetensors
      20G  /data/.../MiniMax-H3/diffusion_models/minimax_h3_fl2va_pruned_int8_convrot.safetensors
      20G  /data/.../Qwen3.6-35B-A3B-GGUF/Qwen3.6-35B-A3B-Q4_K_M.gguf
      19G  /data/.../GLM-5.3-Flash-Uncensored-AWQ/model-00002-of-00009.safetensors


warning: 95 read errors (totals may be low) — run with sudo for full totals
  /root: Permission denied
  /lost+found: Permission denied
  /var/spool/rsyslog: Permission denied
  /var/spool/cups: Permission denied
  /var/spool/cron/crontabs: Permission denied
  /opt/containerd: Permission denied
  /var/tmp/systemd-private-3f6ab085aca7449f934ea3e0d395445e-colord.service-LNFzZE: Permission denied
  /var/tmp/systemd-private-3f6ab085aca7449f934ea3e0d395445e-lm-sensors.service-qquvAn: Permission denied
  /var/tmp/systemd-private-3f6ab085aca7449f934ea3e0d395445e-fwupd.service-WZb05q: Permission denied
  /var/tmp/systemd-private-3f6ab085aca7449f934ea3e0d395445e-bluetooth.service-624Na4: Permission denied
  … 85 more
skipped 37 other filesystems (include with -c)
```

Scanning `/` (2.5M files, 300K dirs) takes ~0.3 s with a warm cache on a 16-core / 32-thread machine.

How it is fast:
- one worker thread per CPU, each walking depth-first on its own stack; work is handed to the
  shared queue only when another thread is idle, so the shared lock is rarely touched
- raw `getdents64` with a 64 KiB buffer, then `statx` with a minimal field mask relative to the directory fd
- entries are stat'ed in inode order, which turns cold-cache inode-table reads into mostly sequential I/O
- per-thread counters, category totals and largest-file heaps, merged once at the end

## Quick install (one shot)

Copy-paste on any Ubuntu/Debian machine — installs build tools, builds, installs to `/usr/local/bin/dirsize`, cleans up, and runs it:

```bash
sudo apt-get install -y git build-essential && d=$(mktemp -d) && git clone --depth 1 https://github.com/mminggmmingg/dirsize.git "$d" && make -C "$d" && sudo make -C "$d" install && rm -rf "$d" && sudo dirsize
```

> The repository is private: `git clone` needs GitHub access (`gh auth login` once, or use an SSH URL).

Update to the latest version: run the same line again. Uninstall:

```bash
sudo rm -f /usr/local/bin/dirsize
```

## Build from a checkout

```sh
make
sudo make install      # /usr/local/bin/dirsize
```

## Usage

```sh
dirsize                 # from /, depth 2, top 10 per directory
sudo dirsize            # also reads root-only dirs and NVMe wear / TBW
dirsize -d 4 -m 1G      # depth 4, hide entries < 1G
dirsize ~ -d 3 -n 5     # home dir, depth 3, top 5 per directory
dirsize -s ~/project    # usage + tree only
dirsize -c /            # cross into other filesystems (mount points)
```

| Option | Description |
|---|---|
| `-d, --depth N` | tree depth to display (default 2). Totals always include everything below |
| `-n, --top N` | show the N largest entries per directory, 0 = all (default 10) |
| `-m, --min SIZE` | hide entries smaller than SIZE (`100M`, `1G`, …) |
| `-f, --files N` | show the N largest files, 0 = off (default 15) |
| `-s, --simple` | usage + tree only (no banner, devices, file types, largest files) |
| `-j, --threads N` | worker threads (default: CPU count). On a cold cache, HDD or NFS, more threads (e.g. `-j 128`) keep more I/O in flight |
| `-c, --cross` | cross into other filesystems |
| `-A, --apparent` | apparent file sizes instead of allocated space |
| `-q, --quiet` | no progress line |
| `--no-color` | disable colors (`NO_COLOR` is honored too) |
| `--no-banner` | hide the banner |

## Sections

- **Storage devices** — physical disks from `/sys/block` (model, type, size) with each partition's filesystem, mount point and usage.
  For NVMe drives: temperature from hwmon (no root needed); with `sudo`, the SMART/Health log adds
  percentage used, spare, total bytes written (TBW), estimated endurance, power-on hours and error counters.
  Without root it shows bytes written since boot instead.
- **Usage** — `df` numbers for the scanned filesystem, split into scanned vs. not scanned
  (permission denied, filesystem metadata, deleted-but-open files), reserved blocks and inode usage.
- **Directory tree** — size-sorted tree; `(files in this dir)` is the size of files directly in that directory.
- **By file type** — totals grouped by extension (AI models, archives, video, code, …).
- **Largest files** — the N biggest files found.

## Notes

- Sizes are allocated space (`st_blocks × 512`), same as `du`. Hard links are counted once; symlinks are never followed.
- By default the scan stays on the starting filesystem, so the total is comparable to that filesystem's `Used` in `df -h`.
  Mount points inside are shown as `[other filesystem]`.
- Do not add up parent and child rows: a directory's size already includes its children.
- The banner is shown only on a terminal at least 96 columns wide.
- Exit code: 0 complete, 1 read errors (partial totals), 2 bad arguments, 130 interrupted.

## License

GPL-3.0 — see [LICENSE](LICENSE). Copyright (C) 2026 mminggmmingg. Modified versions you distribute must also be released as source under GPL-3.0.
