"""The noise fit's measure against a pixel's noise, per develop path (gpu_view.cpp's develop_noise() and its tables).

The fit measures each colour's noise after the develop by what's left after the four neighbours'
mean (/ 1.25: a pixel's variance, for noise that's independent pixel to pixel). The develop's
interpolation makes neighbours alike, so that measure is less than a pixel's variance; and both
are sums over the photosites that went into a pixel, each colour's in proportion to its own
variance. For independent photosite noise of variance s_k (colour k, white balanced, as the
develop reads it), output colour c has

    pixel variance    P_c = sum_k W_ck s_k
    measured          M_c = sum_k V_ck s_k

W_ck the sum of the squared interpolation weights on colour k's photosites (averaged over the
pattern), V_ck the same through the measure's residual operator too: exact for a Bayer mosaic's
Malvar-He-Cutler, which is linear (bayer_exact()). The photosites' s_k are modelled as one
sensor's under every colour, scaled by the white balance the develop applies first (a by mul_k, b
by mul_k^2); each colour's fitted a and b go to a pixel's by P_c / M_c at that s, and a colour
with no fit of its own takes green's by M_c / M_g there. Not s = V^-1 M from the three fits:
W V^-1 leans on the measures' differences (printed below), so a fit's error is amplified.
X-Trans's weights follow the data, so no fixed W and V model it (each colour's photosites alone,
then summed, is printed against the whole): P_c / M_c and M_c / M_g are simulated through its
own interpolation over a grid of red's and blue's photosite noise against green's.

Canon's sRAW and mRAW have no photosites of a colour: luma a pixel, colour filled from the 25
blocks around, whose noise the measure can't see (and a pixel barely has): taken as luma's
alone, a per-colour factor, for our fill alone (the camera's own scaling to an mRAW makes neighbours'
luma alike too, which these can't know: the viewer measures those, as it does LibRaw's develop and a
linear raw's, from the noise's spatial correlation, gpu_view.cpp's measured_law).

python3 tools/noise_paths.py [N [seed [quick]]]: prints Bayer's W, V (and the simulation against
them at unequal noise), the colours' correlations per path, X-Trans's grids and sRAW's factors.
"""
import math, random, sys


def measure(img, N, margin=6):
    """Each colour's pixel variance and the fit's measure of it."""
    out = []
    for c in range(3):
        tot = n = tv = 0.0
        mean = sum(img[y][x][c] for y in range(margin, N - margin) for x in range(margin, N - margin)) / (N - 2 * margin) ** 2
        for y in range(margin, N - margin):
            for x in range(margin, N - margin):
                d = img[y][x][c] - (img[y][x - 1][c] + img[y][x + 1][c] + img[y - 1][x][c] + img[y + 1][x][c]) / 4
                tot += d * d; n += 1; tv += (img[y][x][c] - mean) ** 2
        out.append((tv / n, tot / n / 1.25))
    return out


def bayer(N, rnd, sigma):
    col = lambda x, y: 0 if (x % 2 == 0 and y % 2 == 0) else 2 if (x % 2 and y % 2) else 1
    m = [[sigma[col(x, y)] * rnd.gauss(0, 1) for x in range(N)] for y in range(N)]
    s = lambda x, y: m[min(max(y, 0), N - 1)][min(max(x, 0), N - 1)]
    def px(x, y):
        c = col(x, y); mm = s(x, y)
        cross = s(x-1, y) + s(x+1, y) + s(x, y-1) + s(x, y+1)
        far = s(x-2, y) + s(x+2, y) + s(x, y-2) + s(x, y+2)
        diag = s(x-1, y-1) + s(x+1, y-1) + s(x-1, y+1) + s(x+1, y+1)
        if c == 1:
            h = (5*mm + 4*(s(x-1, y) + s(x+1, y)) - diag - (s(x-2, y) + s(x+2, y)) + 0.5*(s(x, y-2) + s(x, y+2))) / 8
            v = (5*mm + 4*(s(x, y-1) + s(x, y+1)) - diag - (s(x, y-2) + s(x, y+2)) + 0.5*(s(x-2, y) + s(x+2, y))) / 8
            rr = col(x+1, y) == 0
            return (h if rr else v, mm, v if rr else h)
        g = (4*mm + 2*cross - far) / 8; o = (6*mm + 2*diag - 1.5*far) / 8
        return (mm, g, o) if c == 0 else (o, g, mm)
    return [[px(x, y) for x in range(N)] for y in range(N)]


XT = [[1,1,0,1,1,2],[1,1,2,1,1,0],[2,0,1,0,2,1],[1,1,2,1,1,0],[1,1,0,1,1,2],[0,2,1,2,0,1]]
def xtrans(N, rnd, sigma, level=0.1, unit=0.005):
    col = lambda x, y: XT[y % 6][x % 6]
    m = [[level + unit * sigma[col(x, y)] * rnd.gauss(0, 1) for x in range(N)] for y in range(N)]
    inside = lambda x, y: 0 <= x < N and 0 <= y < N
    s = lambda x, y: m[y][x]
    def nearest(p, d, c, reach):
        for k in range(1, reach + 1):
            q = (p[0] + k * d[0], p[1] + k * d[1])
            if not inside(*q): return 0
            if col(*q) == c: return k
        return 0
    def green_at(x, y):
        c = col(x, y); own = s(x, y)
        if c == 1: return own
        lo, hi = 1e30, -1e30
        for j in (-1, 0, 1):
            for i in (-1, 0, 1):
                if inside(x+i, y+j) and col(x+i, y+j) == 1:
                    lo = min(lo, s(x+i, y+j)); hi = max(hi, s(x+i, y+j))
        tot = wts = 0.0
        for d in ((1, 0), (0, 1), (1, 1), (1, -1)):
            a = nearest((x, y), d, 1, 3); b = nearest((x, y), (-d[0], -d[1]), 1, 3)
            if not a or not b: continue
            ga = s(x + a*d[0], y + a*d[1]); gb = s(x - b*d[0], y - b*d[1])
            g = (b*ga + a*gb) / (a + b)
            S = nearest((x, y), d, c, 6); T = nearest((x, y), (-d[0], -d[1]), c, 6)
            if S and T:
                g += a*b / (S*T) * (own - (T*s(x + S*d[0], y + S*d[1]) + S*s(x - T*d[0], y - T*d[1])) / (S + T))
            if lo <= hi: g = min(max(g, lo), hi)
            change = abs(ga - gb) / (a + b); n = 1
            for j in (-1, 0, 1):
                for i in (-1, 0, 1):
                    q1 = (x+i+d[0], y+j+d[1]); q2 = (x+i-d[0], y+j-d[1])
                    if inside(*q1) and inside(*q2) and col(*q1) == col(*q2):
                        change += abs(s(*q1) - s(*q2)) / 2; n += 1
            w = 1 / (1e-10 + (change / n) ** 2)
            tot += w * g; wts += w
        return tot / wts if wts > 0 else ((lo + hi) / 2 if lo <= hi else own)
    G = [[green_at(x, y) for x in range(N)] for y in range(N)]
    def diff(x, y, k, g):
        tot = wts = 0.0
        for j in range(-2, 3):
            for i in range(-2, 3):
                if not inside(x+i, y+j) or col(x+i, y+j) != k: continue
                gq = G[y+j][x+i]
                w = 1 / ((i*i + j*j) * (1e-4 + 0.005*(g + gq) + abs(g - gq)))
                tot += w * (s(x+i, y+j) - gq); wts += w
        return g + tot / wts if wts > 0 else g
    def px(x, y):
        c = col(x, y); own = s(x, y); g = own if c == 1 else G[y][x]
        return (own if c == 0 else diff(x, y, 0, g), g, own if c == 2 else diff(x, y, 2, g))
    img = [[px(x, y) for x in range(N)] for y in range(N)]
    # In units of the photosite noise.
    return [[tuple(v / unit for v in p) for p in row] for row in img]


def ycc(N, rnd, bh, sy, sc, eps):
    # Luma a photosite, colour (Cb, Cr) a block 2 wide, bh tall, filled by develop.hlsl's guided fit.
    Y = [[rnd.gauss(0, sy) for x in range(N)] for y in range(N)]
    C = {}
    for by in range(N // bh + 2):
        for bx in range(N // 2 + 2):
            C[(bx, by)] = (rnd.gauss(0, sc), rnd.gauss(0, sc))
    lum = lambda x, y: Y[min(max(y, 0), N-1)][min(max(x, 0), N-1)]
    def px(x, y):
        bx, by = x // 2, y // bh
        n = sy_ = syy = 0.0; scb = scr = sycb = sycr = 0.0
        for j in range(-2, 3):
            for i in range(-2, 3):
                cx, cy = (bx + i) * 2, (by + j) * bh
                if cx < 0 or cy < 0 or cx >= N or cy >= N: continue
                m = lum(cx, cy) + lum(cx + 1, cy)
                m = (m + lum(cx, cy + 1) + lum(cx + 1, cy + 1)) / 4 if bh == 2 else m / 2
                cb, cr = C[(bx + i, by + j)]
                n += 1; sy_ += m; syy += m * m; scb += cb; scr += cr; sycb += m * cb; sycr += m * cr
        my = sy_ / n; vy = syy / n - my * my
        a1 = (sycb / n - my * scb / n) / (vy + eps); a2 = (sycr / n - my * scr / n) / (vy + eps)
        cb = a1 * Y[y][x] + scb / n - a1 * my; cr = a2 * Y[y][x] + scr / n - a2 * my
        yy = Y[y][x]
        return (yy + cr, yy + (-778 * cb - 2048 * cr) / 4096, yy + cb)
    return [[px(x, y) for x in range(N)] for y in range(N)]


def inverse3(m):
    a, b, c = m[0]; d, e, f = m[1]; g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def mul3(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def matrices(path, N, seed):
    """W and V: column k from plain noise of variance 1 in colour k's photosites alone."""
    W = [[0.0] * 3 for _ in range(3)]; V = [[0.0] * 3 for _ in range(3)]
    for k in range(3):
        sigma = [0.0, 0.0, 0.0]; sigma[k] = 1.0
        res = measure(path(N, random.Random(seed + k), sigma), N)
        for c in range(3):
            W[c][k], V[c][k] = res[c]
    return W, V


def fmt(m):
    return "{" + ", ".join("{" + ", ".join("%.4f" % v for v in row) + "}" for row in m) + "}"


def bayer_exact():
    """W and V for Malvar-He-Cutler exactly, from its weights: each output colour at each of the
    pattern's four sites as a sum over the photosites around, alone and less its four neighbours'
    mean (each neighbour its own site's sum), squared weights summed by photosite colour and
    averaged over the four sites."""
    N = 24
    col = lambda x, y: 0 if (x % 2 == 0 and y % 2 == 0) else 2 if (x % 2 and y % 2) else 1
    def px(m, x, y):
        s = lambda x, y: m[min(max(y, 0), N - 1)][min(max(x, 0), N - 1)]
        c = col(x, y); mm = s(x, y)
        cross = s(x-1, y) + s(x+1, y) + s(x, y-1) + s(x, y+1)
        far = s(x-2, y) + s(x+2, y) + s(x, y-2) + s(x, y+2)
        diag = s(x-1, y-1) + s(x+1, y-1) + s(x-1, y+1) + s(x+1, y+1)
        if c == 1:
            h = (5*mm + 4*(s(x-1, y) + s(x+1, y)) - diag - (s(x-2, y) + s(x+2, y)) + 0.5*(s(x, y-2) + s(x, y+2))) / 8
            v = (5*mm + 4*(s(x, y-1) + s(x, y+1)) - diag - (s(x, y-2) + s(x, y+2)) + 0.5*(s(x-2, y) + s(x+2, y))) / 8
            rr = col(x+1, y) == 0
            return (h if rr else v, mm, v if rr else h)
        g = (4*mm + 2*cross - far) / 8; o = (6*mm + 2*diag - 1.5*far) / 8
        return (mm, g, o) if c == 0 else (o, g, mm)
    W = [[0.0] * 3 for _ in range(3)]; V = [[0.0] * 3 for _ in range(3)]
    X = [[[0.0] * 3 for _ in range(3)] for _ in range(3)]  # X[c][d][k]: output colours c, d's covariance from colour k's photosites
    for (x0, y0) in ((10, 10), (11, 10), (10, 11), (11, 11)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                m = [[0.0] * N for _ in range(N)]
                m[y0 + dy][x0 + dx] = 1.0
                k = col(x0 + dx, y0 + dy)
                centre = px(m, x0, y0)
                around = [px(m, x0 + i, y0 + j) for i, j in ((1, 0), (-1, 0), (0, 1), (0, -1))]
                for c in range(3):
                    w = centre[c]
                    r = w - sum(a[c] for a in around) / 4
                    W[c][k] += w * w / 4
                    V[c][k] += r * r / 4 / 1.25
                    for d in range(3):
                        X[c][d][k] += centre[c] * centre[d] / 4
    bayer_exact.X = X
    return W, V


def ratio(W, V, s):
    return [sum(W[c][k] * s[k] for k in range(3)) / sum(V[c][k] * s[k] for k in range(3)) for c in range(3)]


if __name__ == "__main__":
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 144
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 13
    W, V = bayer_exact()
    print("bayer W %s" % fmt(W))
    print("bayer V %s" % fmt(V))
    for sigma in ((1, 1, 1), (2, 1, 1), (1, 1, 2)):
        res = measure(bayer(N, random.Random(seed), list(sigma)), N)
        print("  sigma %s: simulated pixel / measured %s; from W, V %s" % (sigma, ["%.3f" % (P / M) for P, M in res],
              ["%.3f" % v for v in ratio(W, V, [v * v for v in sigma])]))
    # Why the fit isn't taken to a pixel's by F = W V^-1 on the three measures: R's and B's rows
    # of V are near mirror images, so F leans on differences of the measures, and a fit's few
    # percent of error in one becomes several times that in another's pixel law.
    F = mul3(W, inverse3(V))
    print("bayer F = W V^-1 %s; its rows' absolute sums %s" % (fmt(F), ["%.2f" % sum(abs(v) for v in row) for row in F]))
    X = bayer_exact.X
    print("bayer X (c, d: k) %s" % ("{" + ", ".join("{" + ", ".join(fmt([X[c][d]]) for d in range(3)) + "}" for c in range(3)) + "}"))
    def correlations(img):
        out = []
        for c, d in ((0, 1), (0, 2), (1, 2)):
            xs = [p[c] for row in img[6:-6] for p in row[6:-6]]; ys = [p[d] for row in img[6:-6] for p in row[6:-6]]
            mx = sum(xs) / len(xs); my = sum(ys) / len(ys)
            cxy = sum((u - mx) * (v - my) for u, v in zip(xs, ys)); cx = sum((u - mx) ** 2 for u in xs); cy = sum((v - my) ** 2 for v in ys)
            out.append(cxy / math.sqrt(cx * cy))
        return out
    for sigma in ((1, 1, 1), (2, 1, 1)):
        print("bayer sim correlation RG RB GB at %s: %s" % (sigma, ["%.3f" % v for v in correlations(bayer(N, random.Random(seed), list(sigma)))]))
        print("xtrans sim correlation RG RB GB at %s: %s" % (sigma, ["%.3f" % v for v in correlations(xtrans(N, random.Random(seed), list(sigma)))]))
    for bh in (1, 2):
        print("ycc 2x%d correlation RG RB GB: %s" % (bh, ["%.3f" % v for v in correlations(ycc(N, random.Random(seed), bh, 0.002, 0.002, 1e-4))]))
    if len(sys.argv) > 3:
        sys.exit(0)
    # X-Trans's weights follow the data: its pixel / measured per colour over a grid of the
    # photosite noise of red and of blue against green's.
    grid = (0.5, 0.75, 1.0, 1.5, 2.0, 3.0)
    rows, greens = [], []
    for sr in grid:
        for sb in grid:
            res = measure(xtrans(N, random.Random(seed), [sr, 1.0, sb]), N)
            rows.append([P / M for P, M in res])
            # And each colour's measure against green's (where a colour has no fit of its own).
            greens.append([M / res[1][1] for P, M in res])
            print("  xtrans sigma R %.2f B %.2f: %s, measures against green's %s" % (sr, sb, ["%.3f" % v for v in rows[-1]],
                  ["%.3f" % v for v in greens[-1]]), flush=True)
    print("xtrans grid (R by B, each R G B):")
    print("{" + ", ".join("{%.3ff, %.3ff, %.3ff}" % tuple(r) for r in rows) + "}")
    print("xtrans grid, measures against green's (R by B, R and B):")
    print("{" + ", ".join("{%.3ff, %.3ff}" % (g[0], g[2]) for g in greens) + "}")
    # Frozen weights (each colour's photosites alone through the interpolation, F = W V^-1)
    # against the whole: X-Trans's interpolation follows the data, so that's no model of it.
    Wx, Vx = matrices(xtrans, N, seed)
    Fx = mul3(Wx, inverse3(Vx))
    for sigma in ((1, 1, 1), (2, 1, 1)):
        res = measure(xtrans(N, random.Random(seed + 100), list(sigma)), N)
        pred = [sum(Fx[c][k] * res[k][1] for k in range(3)) for c in range(3)]
        print("xtrans frozen weights at %s: pixel %s, frozen F M %s" % (sigma, ["%.3f" % r[0] for r in res], ["%.3f" % v for v in pred]))
    for bh in (1, 2):
        res = measure(ycc(N, random.Random(seed), bh, 0.002, 0.002, 1e-4), N)
        print("ycc block 2x%d pixel / measured" % bh, ["%.3f" % (P / M) for P, M in res], "measures against green's",
              ["%.3f" % (M / res[1][1]) for P, M in res])
