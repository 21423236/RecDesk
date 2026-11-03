# Third-party notices

RecDesk's own source remains under the repository's MIT license (see `LICENSE`).

FFmpeg is a separately built executable, not RecDesk-authored code. Its actual license depends on the selected build configuration and enabled libraries. In particular, builds containing libx264 are generally GPL-enabled; do not assume a bundled FFmpeg executable is MIT-licensed or LGPL-only.

The build script copies the chosen executable and saves its `ffmpeg -L` output as `FFMPEG-LICENSE.txt`. Before distributing binaries, verify that build's licenses, include all required license texts and notices, and provide the corresponding source / build information as required by those licenses. The generated diagnostic notice is not by itself a complete distribution-compliance package.

Upstream references:

- FFmpeg project / source: https://ffmpeg.org/
- FFmpeg legal information: https://ffmpeg.org/legal.html
- x264 source: https://code.videolan.org/videolan/x264

No FFmpeg or codec source code is copied into this repository by this migration.
