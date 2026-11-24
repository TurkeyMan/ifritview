// Principal colour axis of a symmetric 2x2 covariance. An isotropic
// covariance has every axis as an eigenvector, so use the coordinate axis
// without asking atan2 for the undefined angle of (0,0).
float detail_colour_angle(float xx, float xy, float yy)
{
    if (xy == 0 && xx == yy)
        return 0;
    return 0.5 * atan2(2 * xy, xx - yy);
}
