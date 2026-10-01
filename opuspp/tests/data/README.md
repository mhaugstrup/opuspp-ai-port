# Benchmark audio

`figaro_overture.s16le`: 15 s of raw PCM (signed 16-bit little-endian, 48 kHz,
interleaved stereo). It is taken from 1:36 of W. A. Mozart, *Le nozze di
Figaro*, K. 492, Overture, performed by the Musopen Symphony. The excerpt was
picked for its wide orchestral stereo image (L/R correlation about 0.3) and
raised by 2 dB (peak about -0.2 dBFS).

- Source: https://commons.wikimedia.org/wiki/File:Mozart_-_Le_nozze_di_Figaro,_K492_-_Overture_(Musopen_Symphony).flac
- License: public domain (Musopen)

The clip is not committed: `tools/fetch_deps.sh` downloads the FLAC,
checks its hash and runs this conversion:

    ffmpeg -ss 96 -t 15 -i "Mozart - Le nozze di Figaro, K492 - Overture (Musopen Symphony).flac" \
           -af volume=2dB -f s16le -acodec pcm_s16le -ac 2 -ar 48000 figaro_overture.s16le
