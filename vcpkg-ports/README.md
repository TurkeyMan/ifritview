Ports overriding vcpkg's own for the Windows build (vcpkg-configuration.json
points here), each its upstream port plus one patch, to drop when upstream
has the fix:

- libjxl: `msvc-sub-overflow.patch`: libjxl 0.12 calls `_sub_overflow_i32`
  on MSVC ≥ 19.37, which MSVC 14.39 doesn't declare there; the portable
  subtraction it falls back to elsewhere is used instead.
