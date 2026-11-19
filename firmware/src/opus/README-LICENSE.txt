libopus (vendored subset)
=========================

These sources are a trimmed copy of **libopus 1.3.1** (fixed-point build),
https://opus-codec.org / https://github.com/xiph/opus

Copyright 2001-2019 Xiph.Org Foundation, Skype Limited, Octasic, Jean-Marc Valin,
CSIRO and other contributors.

License: BSD 3-Clause (see COPYING below).

Only the fixed-point encoder + decoder are kept, plus the small config.h that
the build generates. Nothing here is modified except mechanical include-path
rewriting and symbol renaming; both are produced by
`firmware/tools/vendor_opus.py`, which is the authoritative record of the
transformation. To regenerate this subtree from a clean upstream tarball:

    curl -L -o opus.tar.gz https://codeload.github.com/xiph/opus/tar.gz/refs/tags/v1.3.1
    tar xzf opus.tar.gz
    python firmware/tools/vendor_opus.py <path-to-opus-1.3.1> firmware/src/opus

Rationale: the xiaozhi voice protocol requires Opus in both directions.
ESP32-audioI2S only ships a decoder, and its decoder collides on ten global
lookup tables with upstream libopus, so the vendored copy renames them with an
`rcz_` prefix.

The whole range is never enabled: we run 8 kHz-wideband mono at 16 kHz in and
24 kHz out, low complexity, 60 ms frames. The CELT-only and surround paths are
compiled but never reached; they are kept because the reference encoder calls
into them for rate control bookkeeping.

---

Copyright 2001-2011 Xiph.Org, Skype Limited, Octasic,
                    Jean-Marc Valin, Timothy B. Terriberry,
                    CSIRO, Gregory Maxwell, Mark Borgerding,
                    Erik de Castro Lopo

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
