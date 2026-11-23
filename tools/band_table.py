"""The wavelet's scales as filters, for the noise model's spatial correlation (detail.cpp's kBandAutocorr,
shaders/detail.hlsli's).

The filter's B3 spline (1 4 6 4 1)/16 at step 2^j, across then down, makes each scale's smooth image c_j
from the last: in one dimension c_j = P_j * the image, P_0 = delta, P_{j+1} = P_j * B3 at step 2^j; in two,
P_j x P_j. Scale j's detail is c_j - c_{j+1}. Noise of spatial correlation rho(lx, ly) has in it, per unit
of its variance,

    sum over lags l of rho(l) K(l),  K(l) = A_jj(lx) A_jj(ly) - 2 A_j,j+1(lx) A_j,j+1(ly) + A_j+1,j+1(lx) A_j+1,j+1(ly)

(the coarsest, c_5: A_55(lx) A_55(ly)), where A_ab(l) = sum_m P_a(m) P_b(m + l). This prints A_ab for the
eleven pairs (j, j), j = 0..5, and (j, j + 1), j = 0..4, at lags 0..kBandLags - 1: enough for the
measured correlation's reach (6) and the sharpening Gaussian's twice over (up to 9 each).

python3 tools/band_table.py
"""

LAGS = 25


def b3(step):
    return {-2 * step: 1 / 16, -step: 4 / 16, 0: 6 / 16, step: 4 / 16, 2 * step: 1 / 16}


def convolve(p, q):
    out = {}
    for i, a in p.items():
        for j, b in q.items():
            out[i + j] = out.get(i + j, 0) + a * b
    return out


def main():
    p = [{0: 1.0}]
    for j in range(5):
        p.append(convolve(p[-1], b3(1 << j)))
    pairs = [(j, j) for j in range(6)] + [(j, j + 1) for j in range(5)]
    rows = []
    for a, b in pairs:
        row = []
        for l in range(LAGS):
            row.append(sum(v * p[b].get(m + l, 0) for m, v in p[a].items()))
        rows.append(row)
    # White noise's share at each scale, as a check: K(0, 0).
    for j in range(5):
        jj, jk, kk = rows[j][0], rows[6 + j][0], rows[j + 1][0]
        print("// scale %d: white noise %.6f" % (j, jj * jj - 2 * jk * jk + kk * kk))
    print("// coarsest: %.6f" % (rows[5][0] ** 2))
    print("static const float kBandAutocorr[11][%d] = {" % LAGS)
    for (a, b), row in zip(pairs, rows):
        print("    {" + ", ".join("%.8f" % v for v in row) + "},  // A_%d%d" % (a, b))
    print("};")


main()
