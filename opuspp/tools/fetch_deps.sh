#!/usr/bin/env bash
# Downloads the external inputs that are not committed:
#
#   ../opus-1.6.1/                       libopus reference sources (tests, benchmarks, tools),
#                                        next to the project, or $OPUSPP_LIBOPUS_DIR
#   tests/data/figaro_overture.s16le     benchmark music clip (benchmarks, opuspp_encode)
#
# The library itself is header-only and needs neither. Both downloads are
# hash-checked; existing files are kept.
#
#   tools/fetch_deps.sh [project-root]      (default: this checkout)
#
# Needs curl, sha256sum, tar and ffmpeg.
set -euo pipefail
root=$(cd "${1:-$(dirname "$0")/..}" && pwd)
libopus=${OPUSPP_LIBOPUS_DIR:-$(dirname "$root")/opus-1.6.1}

opus_url=https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
opus_sha=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
flac_url='https://upload.wikimedia.org/wikipedia/commons/d/d2/Mozart_-_Le_nozze_di_Figaro%2C_K492_-_Overture_%28Musopen_Symphony%29.flac'
flac_sha=484cb77a1a5ba764c17128e385058baefc0cf2076c542bc07cb91b05450d121d
clip_sha=85c1987ce018ddf6d0ae27bd68ccd8fc3fa0556cb24b32a4c84e3829ece76faf
# Wikimedia asks automated clients to identify themselves.
user_agent='opuspp-fetch/1.0 (benchmark audio; https://github.com/mhaugstrup/opuspp-ai-port)'

for tool in curl sha256sum tar ffmpeg; do
   command -v $tool >/dev/null || { echo "fetch_deps.sh needs $tool" >&2; exit 1; }
done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [[ -e $libopus ]]; then
   echo "$libopus already present"
else
   echo "fetching libopus 1.6.1"
   curl -fsSL "$opus_url" -o "$tmp/opus.tar.gz"
   echo "$opus_sha  $tmp/opus.tar.gz" | sha256sum -c --quiet
   mkdir -p "$libopus"
   tar xzf "$tmp/opus.tar.gz" -C "$libopus" --strip-components=1
fi

clip=$root/tests/data/figaro_overture.s16le
if [[ -e $clip ]]; then
   echo "tests/data/figaro_overture.s16le already present"
else
   echo "fetching the benchmark music (Wikimedia Commons, public domain)"
   mkdir -p "$root/tests/data"
   curl -fsSL -A "$user_agent" "$flac_url" -o "$tmp/music.flac"
   echo "$flac_sha  $tmp/music.flac" | sha256sum -c --quiet
   # Recipe from tests/data/README.md.
   ffmpeg -loglevel error -y -ss 96 -t 15 -i "$tmp/music.flac" -af volume=2dB \
          -f s16le -acodec pcm_s16le -ac 2 -ar 48000 "$clip"
   if ! echo "$clip_sha  $clip" | sha256sum -c --quiet 2>/dev/null; then
      # The FLAC is verified, so a mismatch can only come from the conversion.
      echo "warning: $(ffmpeg -version | head -1) converted the clip differently from the" >&2
      echo "         original (ffmpeg 8.1); the benchmark input differs slightly." >&2
   fi
fi
