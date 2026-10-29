# lpmap

Expose the logical partitions of an Android dynamic-partitions `super` image or
device (`system`, `vendor`, `product`, ...) as loop devices, from a minimal
initramfs. It only reads the LP metadata; nothing is written to `super`.

Written for a Galaxy A51 mainline port, where `system` lives inside `super` and
there is no `dmsetup`. Freestanding C with raw aarch64 syscalls: no libc, one
static binary of a few KiB.

## Usage

```
lpmap [-s slot] [-r|-w] <super-dev> list
lpmap [-s slot] [-r|-w] <super-dev> map [name...]
```

`map` attaches `/dev/loopN` at the extent's offset/size and creates symlinks

- `/dev/<super-base>p<index>` (index = 1-based position in the LP table, e.g. `/dev/sda25p1`)
- `/dev/lp/<name>`

Then `mount /dev/lp/system /mnt`. The kernel cannot create real `sdaXpY` nodes
for nested partitions, so these are symlinks to loop devices.

- Partitions with the LP readonly attribute map read-only unless `-w` is given.
  `-r` forces read-only.
- Only partitions with exactly one linear extent are mapped (others are skipped
  with a message). Multi-extent partitions would need dm-linear.
- `-s` selects the metadata slot (default 0). The header checksum is not verified.

Needs `CONFIG_BLK_DEV_LOOP` and devtmpfs (`/dev/loop-control`).

## Build

```
make    # clang + lld, aarch64 target; no sysroot needed
```
