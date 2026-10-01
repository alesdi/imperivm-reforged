# stb_vorbis

The Ogg Vorbis decoder for the installation's music (`music/*.ogg`), used by
`engine/sound/src/ogg.cpp` and by nothing else. See `docs/engine/sound.md`.

* **Upstream:** <https://github.com/nothings/stb>, file `stb_vorbis.c`.
* **Version:** v1.22, as the file's own first line says.
* **Fetched at:** commit `2c980bb59875b0d32144a71867fbdebb2f77cd20` of the
  repository's `master`. The file's last change upstream is commit
  `1ee679ca2ef753a528db5ba6801e1067b40481b8` ("update version numbers"); the
  copy here is byte-identical to both.
* **SHA-256 of `stb_vorbis.c`:**
  `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`.
* **Licence:** public domain (the Unlicense) or MIT, at the user's choice;
  `LICENSE` is the repository's, and the same text closes `stb_vorbis.c`.
  Either is compatible with this project's GPL-3.0-or-later.
* **Local changes:** none. Update by replacing the file and these lines.

It builds as its own static library (`CMakeLists.txt` here), C99, with its
warnings off, and with `STB_VORBIS_NO_STDIO` and `STB_VORBIS_NO_PUSHDATA_API`:
the engine hands it bytes and pulls samples.
