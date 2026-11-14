# bak

A tiny, careful backup tool. Pick some folders, and `bak` packs them into one
compressed (optionally encrypted) file in a folder you choose, and can upload it.

It is deliberately small: one C file, libc only. It uses the standard tools
`tar`, `xz`, `gpg` and `rclone` to do the work. It does not do incremental
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

You need `tar` and `xz`. You need `gpg` only for encryption and `rclone` only for uploads.

## Use

```bash
bak ~/Documents ~/Pictures
```

Folders given on the command line are added to the selection. The menu then lets you:

| key | action |
|-----|--------|
| a / r | add / remove a folder |
| d | destination folder |
| n | file name prefix (default `backup`) |
| c | compression: 1 core (smallest file, slow) or all cores (faster, about 1% bigger) |
| e | encryption on/off (gpg, AES-256, passphrase) |
| u | upload target as an rclone `remote:path` (set the remote up with `rclone config`) |
| k | keep or delete the local file after a verified upload |
| s | check everything, then start |

Output: `PREFIX-YYYYMMDD-HHMMSS.tar.xz` (or `.tar.xz.gpg`), mode 0600.

## Safety

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
3. Only then is it renamed (an atomic operation) to its final name.
4. If anything fails, warns or is interrupted (Ctrl-C), even a file changing
   while it is being read, the partial file is deleted. **A file with the
   final name is always a complete, verified backup.**
5. Uploads are checked with `rclone check`. The local copy is deleted only
   after the upload has been verified.

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
