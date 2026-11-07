---
name: Bug report
about: Create a report to help us improve
title: ''
labels: bug
assignees: ''

---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Set '...' in `camera.conf`
2. Boot the camera / connect with '...'
3. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Environment**
- buddy3d-local version (`buddy3d/VERSION` on the SD card, or the release name):
- Stock camera firmware version (`cat /oem/usr/sbin/fw_version.txt` on the camera):
- Client used, if relevant (e.g. VLC, ffmpeg, go2rtc / Home Assistant, Scrypted):
- SD card size and format:

**Configuration**
The relevant lines of your `camera.conf`.
**Remove `wifi_psk` and anything else private before pasting.**

```
```

**Logs**
Attach or paste the relevant parts of `buddy3d/logs/boot.log` and
`buddy3d/logs/camd.log` from the SD card (set `debug=1` in `camera.conf` for a
traced boot log). Check them for private data first.

**Additional context**
Add any other context about the problem here, e.g. a frame grab from the stream.
