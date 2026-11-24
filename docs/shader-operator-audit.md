# Shader matrix and conditional semantics audit

Audited the application `.hlsl` / `.hlsli` files at master `36d5784`.
Matrix declarations occur only in `detail.hlsli`, `display.hlsli`,
`stats.hlsl` and `view.hlsli`. There are no matrix type aliases or other
half/double/generic matrix declarations in those shaders.

## Every matrix-typed multiplication operator

| Function / expression | Operand types | Mathematical operation | Disposition |
|---|---|---|---|
| `detail_kept`: `unit * detail_pairs(kk)` | 3x3, 3x3 | Corresponding covariance components (Hadamard) | `detail_component_product` |
| `detail_kept`: `unit * detail_pairs(dd)` | 3x3, 3x3 | Corresponding sharpening components | `detail_component_product` |
| `detail_kept`: `unit * detail_pairs(xx)` | 3x3, 3x3 | Corresponding cross-covariance components | `detail_component_product` |
| `detail_kept`: `unit * detail_pairs(w.D)` | 3x3, 3x3 | Corresponding whole high-pass components | `detail_component_product` |
| `detail_kept`: `unit * detail_pairs(w.X)` | 3x3, 3x3 | Corresponding whole cross-covariance components | `detail_component_product` |
| `detail_retained`: `l1 * (...)`, `m.x * float2x2(...)`, `m.y * float2x2(...)` | Scalar, 2x2 (three occurrences) | Scale a covariance / eigenprojector | Unchanged; unambiguous scalar scaling |
| `detail_retained`: `m.z * float2x2(...)`, `m.w * float2x2(...)` | Scalar, 2x2 (two occurrences) | Scale a Jacobian eigenprojector | Unchanged; unambiguous scalar scaling |
| `detail_power`: `f * float3x3(...)` | Scalar, 3x3 | Scale an eigenprojector | Unchanged; unambiguous scalar scaling |
| `detail_transport`: `(m + transpose(m)) * .5` | 3x3, scalar | Symmetrize by averaging | Unchanged; unambiguous scalar scaling |

These are all five matrix/matrix `*` sites and all seven scalar/matrix
`*` sites. The component-product helper multiplies explicit `float3` rows,
so each remaining `*` in that helper is vector/vector component arithmetic.
The other apparent outer products construct matrices from scalar/vector
row products; their operands are not matrices. Matrix division in
`detail_eigen` is division by scalar 2.

Every product with matrix operands that represents linear algebra already
uses `mul`: the covariance changes of
basis, transport and inverse factors, scale Jacobians, covariance-vector
products, display colour matrices, Oklab transforms and the statistics
input transform. No matrix product was changed into a Hadamard product or
vice versa. The stored covariance units and measured per-pair factors
still have exactly the CPU model's corresponding-component multiplication.

HLSL defines ordinary matrix arithmetic per component; linear-algebra
multiplication uses `mul`. The glslang 16.6.0 minimal reproduction emits
`OpMatrixTimesMatrix` for matrix `*`, whereas DXC emits component `OpFMul`.
Explicit row products keep the intended operation visible in both.

References: [HLSL per-component operations](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-per-component-math),
[HLSL mul](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-mul),
[DXC SPIR-V expression mapping](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst#hlsl-expressions).

## The shrinker's out parameter

The coarsest scale is not shrunk. Its retained covariance is `bj` and its
Jacobian must stay identity. The old expression
`j < 5 ? detail_retained(..., gj) : bj` depended on the unselected call not
writing `gj`. Legacy HLSL evaluates both conditional operands; HLSL 2021
introduced short-circuiting for scalar conditions. This is a language-version
portability error, distinct from glslang's matrix-operation lowering.

Initialize `rj = bj`, then call the shrinker only inside `if (j < 5)`.
The CPU model's control flow and all covariance formulas stay unchanged.

In the six-case isolated Vulkan probe, glslang and DXC `-HV 2018` select the
correct returned covariance but overwrite the coarsest out parameter.
DXC's default / `-HV 2021` preserve identity. Explicit `if` preserves it in
both tested compilers. All probes use the same runtime data, runner and
llvmpipe device.

Reference: [Microsoft HLSL 2021 migration guide](https://devblogs.microsoft.com/directx/hlsl-2021-migration-guide/).

The regression is the compiled GPU `--check-detail` shaping and quiet-axis
table comparison against the production CPU model, including the active
endpoint's 0.005-sigma bound. It exercises actual non-diagonal covariance
and all six scales rather than only a diagonal multiplication example.

## After the CPU table migration

The current pixel program no longer contains the per-image TABLE or
SHAPING runtime modes. Workers use the shared CPU shaping and decode-table
implementation instead. The audited helper definitions remain in
`detail.hlsli`, with the explicit component products and coarsest-scale
`if` preserved; removing their old callers does not revert their semantics.
The current `--check-detail` table gate checks the CPU snapshot/worker path,
uploaded texture and quiet-axis control. The earlier GPU-table comparison
above records the shader-semantic regression that motivated this audit.
