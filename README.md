# dirsize

Fast parallel disk-usage tree for Linux, written in C (pthreads).
Starts at `/` by default and shows where the space in `df -h` actually goes.

```
Filesystem      /dev/nvme0n1p2           Mounted on /
Size 1.9T   Used 1.3T   Avail 544G   Use% 70%
스캔 합계 1.1T (df Used 의 85.9%) · 0.32s · 폴더 306104 · 파일 2562921 · 스레드 64

   1.1T 100.0% ██████████  /
   755G  71.0% ███████░░░  ├── data/
   755G  71.0% ███████░░░  │   └── parksThink/
   175G  16.4% ██░░░░░░░░  ├── home/
   ...
```

## Build / install

```sh
make
sudo make install      # /usr/local/bin/dirsize
```

## Usage

```sh
dirsize                 # / 부터, 깊이 2, 폴더마다 상위 10개
sudo dirsize            # 권한 없는 폴더까지 전부 (df Used 와 거의 일치)
dirsize -d 4 -m 1G      # 깊이 4, 1G 미만 숨김
dirsize ~ -d 3 -n 5     # 홈 폴더, 깊이 3, 폴더마다 상위 5개
dirsize -c /            # 다른 파일시스템(마운트)까지 포함
```

| 옵션 | 설명 |
|---|---|
| `-d, --depth N` | 트리 표시 깊이 (기본 2). 용량은 항상 하위 전체를 합산 |
| `-n, --top N` | 폴더마다 큰 항목 N개만 표시, 0 = 전부 (기본 10) |
| `-m, --min SIZE` | SIZE 미만 항목 숨김 (`100M`, `1G` …) |
| `-j, --threads N` | 스레드 수 (기본 CPU×2, 최대 64) |
| `-c, --cross` | 마운트 경계를 넘어 다른 파일시스템도 탐색 |
| `-A, --apparent` | 할당 공간 대신 논리적 파일 크기 |
| `-q, --quiet` | 진행 표시 끄기 |
| `--no-color` | 색상 끄기 (`NO_COLOR` 환경변수도 지원) |

## Notes

- Sizes are allocated space (`st_blocks × 512`), same as `du`. Hard links are counted once; symlinks are never followed.
- By default the scan stays on the starting path's filesystem, so the total is comparable to that filesystem's `Used` in `df -h`. Mount points inside are shown as `[다른 파일시스템]`.
- The gap to `df Used` comes from unreadable directories (run with `sudo`), deleted-but-open files, and filesystem metadata/reserved blocks.
- Exit code: 0 complete, 1 read errors (partial totals), 2 bad arguments, 130 interrupted.
