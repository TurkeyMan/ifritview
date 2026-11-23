"""The colour shrinker's moments (detail.hlsli's kShrinkMoments, detail.cpp's twin), and the
B3 a trous scales' shares of a pixel's plain noise.

Colour noise reduction shrinks each scale's colour-difference pair (Cr, Cb) as a 2-vector by its
length: s(u) = u max(0, 1 - t / |u|). For a Gaussian pair, in its covariance's eigenframe with
the larger variance 1 and the other kappa, and t = tau (in units of the larger standard
deviation), this prints for tau = 0, 0.25 .. 8 and kappa = 0, 0.125 .. 1:

    m1 = E[s1^2], m2 = E[s2^2]           what's kept of each component's variance
    j1 = E[ds1/du1], j2 = E[ds2/du2]      the mean Jacobian (what a correlated luminance's
                                          covariance with the pair keeps, by Stein's lemma)

(the cross terms vanish by symmetry), by the trapezoidal rule over +-7 standard deviations, 401
points a side. python3 tools/shrink_table.py
"""
import math

def moments(tau, kappa, n=401, reach=7.0):
    s2 = math.sqrt(kappa)
    xs = [-reach + 2 * reach * i / (n - 1) for i in range(n)]
    wx = [math.exp(-0.5 * x * x) for x in xs]
    if kappa > 0:
        ys = [s2 * x for x in xs]; wy = wx
    else:
        ys = [0.0]; wy = [1.0]
    m1 = m2 = j1 = j2 = tot = 0.0
    for x, a in zip(xs, wx):
        for y, b in zip(ys, wy):
            w = a * b
            tot += w
            r = math.hypot(x, y)
            if tau == 0:
                m1 += w * x * x; m2 += w * y * y; j1 += w; j2 += w
                continue
            if r <= tau:
                continue
            f = 1 - tau / r
            m1 += w * (x * f) ** 2
            m2 += w * (y * f) ** 2
            j1 += w * (f + tau * x * x / r ** 3)
            j2 += w * (f + tau * y * y / r ** 3)
    return m1 / tot, m2 / tot, j1 / tot, j2 / tot

def b3_shares():
    h = [1 / 16, 4 / 16, 6 / 16, 4 / 16, 1 / 16]
    def conv(a, b):
        r = [0.0] * (len(a) + len(b) - 1)
        for i, x in enumerate(a):
            for j, y in enumerate(b):
                r[i + j] += x * y
        return r
    def dil(k, s):
        r = [0.0] * ((len(k) - 1) * s + 1)
        for i, x in enumerate(k):
            r[i * s] = x
        return r
    g = [1.0]
    for j in range(5):
        g = conv(g, dil(h, 2 ** j))
    e = sum(v * v for v in g)
    return e * e  # the coarsest approximation's variance, 2D, plain noise of 1

if __name__ == "__main__":
    print("coarsest approximation's variance (plain noise 1): %.6f" % b3_shares())
    rows = []
    for ti in range(33):
        for ki in range(9):
            rows.append(moments(ti * 0.25, ki / 8))
    print("{" + ",\n ".join("{%.6f, %.6f, %.6f, %.6f}" % r for r in rows) + "}")
