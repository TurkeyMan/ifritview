# etcdec

`etcdec.h` from https://github.com/iOrange/etcdec (v0.91, commit
972875d403ed8ac27e0f35c2f29d819e710a688a), unmodified but for a dropped byte
order mark. Decodes ETC1, ETC2 (RGB, punch-through alpha, RGBA) and EAC
R11/RG11 blocks; used by the PKM, KTX and KTX2 decoders through
`src/decoders/etc.cpp`. Its signed EAC path reads the blocks as unsigned
ones, so `etc.cpp` decodes signed R11/RG11 itself.

Licence: MIT or the Unlicense, at your choice (see the end of the header).
