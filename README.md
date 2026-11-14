# bak

A tiny, careful backup tool. Pick some folders, and `bak` packs them into one
compressed (optionally encrypted) file in a folder you choose, and can upload it.

It is deliberately small: one C file, using only libc and ncurses. It uses the
standard tools `tar`, `xz`, `gpg` and `rclone` to do the work. It does not do incremental
backups, scheduling or restores.

## Build

```bash
make
```

```bash
sudo make install
```

### Arch Linux (pacman)

```bash
git clone https://github.com/yorishori/bak && cd bak && makepkg -si
```

`makepkg` builds the package from the latest code on GitHub and installs it
through pacman, so `pacman -R bak` removes it. To update later, run
`makepkg -si` again in the same folder.

You need `ncurses`, `tar` and `xz`. You need `gpg` only for encryption and `rclone` only for uploads.

## Use

```bash
bak
```

```bash
bak ~/Documents ~/Pictures
```

Folders given on the command line are added to the list. Everything works with
the keyboard or the mouse: click a button, setting or folder, or use the keys
shown in brackets.

**Main screen**

- **Folders to back up**: each folder is listed with its size and number of
  files. Folders with unreadable files are marked in red.
  - `a`: add a folder (opens the folder picker)
  - `r` or `Del`, or click `[x]`: remove the selected folder from the list
- **Settings**: press `Enter` on a setting, click it, or press its key:
  - `d` Destination: a folder picker. `m` jumps to your mounted drives
    (`/run/media/$USER`, `/media`, `/mnt`).
  - `c` Compression:
    - *Smallest*: `xz -9e`, 1 thread. This is the default.
    - *Small*: `xz -9e`, all cores.
    - *Fast*: `xz -6`, all cores.
  - `e` Encryption: off, or gpg AES-256 with a passphrase you type when the
    backup starts.
  - `u` Upload: choose one of your rclone remotes (set up with `rclone config`)
    and a folder on it.
  - `n` File name prefix (default `backup`).
- `s` starts: everything is checked first, then you see the log, a progress bar
  and the time left. `Esc` aborts at any time.

**Folder picker**

| key | action |
|-----|--------|
| `Enter` / double-click | open folder (on the first line: use the current folder) |
| `Space` | select the highlighted folder |
| `Bksp` / `Left` | go up |
| `~` / `m` | jump home / to mounted drives |
| `/` | type a path |
| `.` | show hidden folders |
| `Esc` | cancel |

Output: `PREFIX-YYYYMMDD-HHMMSS.tar.xz` (or `.tar.xz.gpg`), read-only (mode 0400).

## Safety

The folders being backed up are only ever read. `bak` never writes, moves or
deletes anything in them.

Before anything is written, `bak` checks:

- the required programs are installed
- every source is a folder, and every file and folder inside it can be read
- no two sources share a name, and no source contains another
- the destination exists, is writable, and is not inside a source
- free space (warns if it is less than the uncompressed size)
- FAT32 destinations (warns about the 4 GiB file limit)
- the output name does not already exist, and the rclone remote is configured

During the backup:

1. The archive is written to a hidden `.NAME.partial` file and flushed to disk (`fsync`).
2. It is then verified in full: decrypted (if encrypted), decompressed, and
   every tar header and file is read. Every layer has its own integrity check:
   xz uses SHA-256 and gpg uses MDC.
3. Only then is it made read-only and renamed (an atomic operation) to its final name.
4. If anything fails, warns or is aborted (`Esc`), even a file changing
   while it is being read, the partial file is deleted. **A file with the
   final name is always a complete, verified backup.**
5. Uploads are checked with `rclone check`. The local backup is always kept.

If the process is killed hard (power cut, `kill -9`), a `.partial` file may be
left behind. It is incomplete; delete it.

## Restore

```bash
xz -dc backup-....tar.xz | tar -xf -
```

```bash
gpg -d backup-....tar.xz.gpg | xz -dc | tar -xf -
```

Both commands extract into the current folder, recreating each backed-up folder by name.
