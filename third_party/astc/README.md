# ASTC decoder

`android_astc_decomp.cpp` from Basis Universal
(https://github.com/BinomialLLC/basis_universal, v2.5, commit
ee771a7f913a3233a252eb6d281b4446f6a3c9f5, `encoder/3rdparty/`), unmodified.
It is the ASTC decoder of Android's drawElements Quality Program (dEQP, the
GPU conformance tests), with its dependencies on the rest of dEQP removed
and a void-extent fix by Binomial. Decodes 2D ASTC blocks of every
footprint (4x4 to 12x12), LDR and HDR; used by the .astc, KTX and KTX2
decoders through `src/decoders/astc.cpp`, which includes it and calls its
`decompress_ldr`, `decompress_hdr` and `is_hdr`.

Licence: Apache 2.0 (`LICENSE`), Copyright 2016 The Android Open Source
Project; Basis Universal's attribution notice is in `NOTICE`.
