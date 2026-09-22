# CLAUDE.md

## What this is

Registers Sonos speakers as real, synchronisable LMS players. Not an LMS plugin — a standalone program built on the squeezelite codebase with a Sonos output driver instead of ALSA.

## Build environment

The WSL2 checkout at `/mnt/c/HueSyncLXC/sonos-squeezebox` is the development environment. Build and test locally with `make`. The Makefile supplies the compatibility flags for current compilers and CMake.

LXC 113 (192.168.178.31) is deployment only. Agents must never SSH to it or deploy there. After the agent commits and pushes, the user runs `git pull && make && systemctl restart` on the deployment target.

## Submodules

`noson/` and `squeezelite/` are submodules — leave them untouched. Any change inside them will be lost on the next `git submodule update`.

## Our modifications

- **Track metadata**: LMS CLI (port 9090) is queried for track title and artwork at stream start.
- **MAC address**: sent raw, not URL-encoded. URL-encoding causes LMS to silently fail to match the player.
- **Artwork URL**: constructed as `http://<lms>:9000/music/<id>/cover.jpg` because LMS does not include `artwork_url` for local tracks.
- **Player name**: registered as `<room> (Sonos)` (e.g. `Study (Sonos)`) — human-readable in every LMS controller.

## Rejected approaches

**ICY/Shoutcast in-band metadata** — Sonos never sends the `Icy-MetaData: 1` opt-in header for `audio/flac` streams, so injecting ICY blocks corrupts the stream. See README for details.

## Diagnostics

The program line-buffers stdout, including under systemd. Use `journalctl` for transport/stream logs and `ss -tnp` for connection state. See `DEVICE-VERIFICATION.md` for the physical-device acceptance checks.

## Pitfalls

- Never pass `--server=<ip>:9000`; port 9000 is the web interface. Pass `--server=<ip>` only — slimproto is found on port 3483.

## Conventions

- All documentation and commit messages in English.
- Always push after committing.
