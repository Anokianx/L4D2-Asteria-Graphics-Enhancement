import math

__author__ = "Anokianx"


class UnsupportedNativeView(ValueError):
    pass


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def multiply(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def close_components(a, b, absolute=0.0001, relative=0.0001):
    return len(a) == len(b) and all(math.isfinite(x) and math.isfinite(y) and abs(x - y) <= absolute + relative * max(abs(x), abs(y)) for x, y in zip(a, b))


def recover_native_perspective(values):
    if len(values) != 16 or any(not isinstance(x, (int, float)) or isinstance(x, bool) for x in values):
        raise UnsupportedNativeView("sixteen_numeric_row_major_components_required")
    if not all(math.isfinite(x) for x in values):
        raise UnsupportedNativeView("non_finite_native_matrix")
    rows = [list(values[i:i + 4]) for i in range(0, 16, 4)]
    length_x = math.sqrt(dot(rows[0][:3], rows[0][:3]))
    length_y = math.sqrt(dot(rows[1][:3], rows[1][:3]))
    length_w = math.sqrt(dot(rows[3][:3], rows[3][:3]))
    if abs(length_w - 1.0) > 0.0001:
        raise UnsupportedNativeView("clip_w_basis_is_not_unit_native_perspective")
    if min(length_x, length_y) <= 0.0000001:
        raise UnsupportedNativeView("degenerate_perspective_scale")
    view = [[x / length_x for x in rows[0]], [x / length_y for x in rows[1]], [-x for x in rows[3]], [0.0, 0.0, 0.0, 1.0]]
    basis = [row[:3] for row in view[:3]]
    orthogonal_error = max(abs(dot(basis[r], basis[c]) - float(r == c)) for r in range(3) for c in range(3))
    if orthogonal_error > 0.0001:
        raise UnsupportedNativeView("non_orthonormal_or_offcenter_native_projection")
    determinant = dot(basis[0], cross(basis[1], basis[2]))
    if abs(determinant - 1.0) > 0.0001:
        raise UnsupportedNativeView("unsupported_reflected_or_left_handed_native_basis")
    depth_scale = dot(rows[2][:3], basis[2]) / dot(basis[2], basis[2])
    depth_offset = rows[2][3] - depth_scale * view[2][3]
    if not (depth_scale < -1.0 and depth_offset < 0.0):
        raise UnsupportedNativeView("unsupported_depth_convention_or_clip_range")
    near_plane = depth_offset / depth_scale
    far_plane = depth_offset / (depth_scale + 1.0)
    if not (math.isfinite(near_plane) and math.isfinite(far_plane) and 0.0 < near_plane < far_plane):
        raise UnsupportedNativeView("invalid_clip_planes")
    projection = [[length_x, 0.0, 0.0, 0.0], [0.0, length_y, 0.0, 0.0], [0.0, 0.0, depth_scale, depth_offset], [0.0, 0.0, -1.0, 0.0]]
    recomposed = multiply(projection, view)
    rebuilt = [value for row in recomposed for value in row]
    if not close_components(values, rebuilt, absolute=0.00001, relative=0.000002):
        raise UnsupportedNativeView("native_matrix_recomposition_residual")
    origin = [-sum(view[r][c] * view[r][3] for r in range(3)) for c in range(3)]
    return {"available": True, "diagnostics_only": True, "derived_from_native_draw_constants": True,
            "matrix_layout": "row_major_P_times_V_no_transpose_no_rescale", "world_to_view": view,
            "projection": projection, "origin": origin, "rotation_determinant": determinant,
            "basis_orthogonal_maximum_error": orthogonal_error,
            "near_plane": near_plane, "far_plane": far_plane,
            "far_plane_condition_indicator": 1.0 / abs(depth_scale + 1.0),
            "horizontal_fov_degrees": math.degrees(2.0 * math.atan(1.0 / length_x)),
            "vertical_fov_degrees": math.degrees(2.0 * math.atan(1.0 / length_y)),
            "projection_aspect_ratio": length_y / length_x,
            "recomposition_maximum_absolute_error": max(abs(a - b) for a, b in zip(values, rebuilt)),
            "native_camera_ready": False, "main_view_verified": False, "frame_timing_verified": False,
            "motion_vectors_verified": False}
