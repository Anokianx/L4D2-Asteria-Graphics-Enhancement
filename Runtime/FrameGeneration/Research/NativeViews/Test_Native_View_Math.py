import hashlib
import json
import math
from pathlib import Path
import random
import struct
import tempfile
import unittest

from Analyze_Native_Views import analyze
from Native_View_Math import UnsupportedNativeView, close_components, recover_native_perspective

__author__ = "Anokianx"
HERE = Path(__file__).resolve().parent


def product(a, b):
    return [[sum(a[row][part] * b[part][column] for part in range(4)) for column in range(4)] for row in range(4)]


def rotation(angles):
    x, y, z = angles
    cx, cy, cz = math.cos(x), math.cos(y), math.cos(z)
    sx, sy, sz = math.sin(x), math.sin(y), math.sin(z)
    return [[cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
            [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
            [-sy, cy * sx, cy * cx]]


def make_fixture(origin=(0.0, 0.0, 0.0), angles=(0.0, 0.0, 0.0), fov=90.0, aspect=16.0 / 9.0, near=7.0, far=5000.0):
    basis = rotation(angles)
    view = [basis[row] + [-sum(basis[row][column] * origin[column] for column in range(3))] for row in range(3)] + [[0.0, 0.0, 0.0, 1.0]]
    scale_x = 1.0 / math.tan(math.radians(fov) / 2.0)
    projection = [[scale_x, 0.0, 0.0, 0.0], [0.0, scale_x * aspect, 0.0, 0.0],
                  [0.0, 0.0, far / (near - far), far * near / (near - far)], [0.0, 0.0, -1.0, 0.0]]
    return {"values": [value for row in product(projection, view) for value in row], "view": view,
            "projection": projection, "origin": origin, "fov": fov, "aspect": aspect, "near": near, "far": far}


class NativeViewMathContract(unittest.TestCase):
    analytic_fixture_count = 0

    def test_rotated_translated_known_cameras_and_frustum_planes(self):
        generator = random.Random(502117)
        for index in range(216):
            pose = make_fixture(origin=tuple(generator.uniform(-16000, 16000) for _ in range(3)),
                                angles=tuple(generator.uniform(-math.pi, math.pi) for _ in range(3)),
                                fov=generator.uniform(20, 150), aspect=generator.choice([4 / 3, 16 / 9, 32 / 9]),
                                near=generator.choice([0.5, 2.0, 7.0, 16.0]), far=generator.choice([500.0, 5000.0, 56756.0]))
            with self.subTest(index=index):
                recovered = recover_native_perspective(pose["values"])
                for actual, expected in zip(recovered["origin"], pose["origin"]):
                    self.assertAlmostEqual(actual, expected, places=7)
                self.assertAlmostEqual(recovered["horizontal_fov_degrees"], pose["fov"], places=9)
                self.assertAlmostEqual(recovered["projection_aspect_ratio"], pose["aspect"], places=10)
                self.assertAlmostEqual(recovered["near_plane"] / pose["near"], 1.0, places=7)
                self.assertAlmostEqual(recovered["far_plane"] / pose["far"], 1.0, places=7)
                for r in range(4):
                    for c in range(4):
                        self.assertAlmostEqual(recovered["world_to_view"][r][c], pose["view"][r][c], places=7)
                basis = [row[:3] for row in pose["view"][:3]]
                matrix = [pose["values"][i:i + 4] for i in range(0, 16, 4)]
                for distance, expected_depth in ((pose["near"], 0.0), (pose["far"], 1.0)):
                    for corner_x, corner_y in ((-1.0, -1.0), (-1.0, 1.0), (1.0, -1.0), (1.0, 1.0)):
                        camera = [corner_x * distance * math.tan(math.radians(pose["fov"]) / 2.0),
                                  corner_y * distance * math.tan(math.radians(pose["fov"]) / 2.0) / pose["aspect"], -distance]
                        world = [pose["origin"][c] + sum(basis[r][c] * camera[r] for r in range(3)) for c in range(3)] + [1.0]
                        clip = [sum(matrix[r][c] * world[c] for c in range(4)) for r in range(4)]
                        self.assertAlmostEqual(clip[0] / clip[3], corner_x, places=7)
                        self.assertAlmostEqual(clip[1] / clip[3], corner_y, places=7)
                        self.assertAlmostEqual(clip[2] / clip[3], expected_depth, places=7)
                self.__class__.analytic_fixture_count += 1

    def test_orthographic_matrix_rejected(self):
        values = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, 1.0]
        with self.assertRaisesRegex(UnsupportedNativeView, "clip_w_basis"):
            recover_native_perspective(values)

    def test_no_global_projective_rescaling(self):
        for scale in (0.5, 2.0, -1.0):
            with self.subTest(scale=scale), self.assertRaises(UnsupportedNativeView):
                recover_native_perspective([scale * value for value in make_fixture()["values"]])

    def test_offcenter_and_jitter_rejected(self):
        fixture = make_fixture(origin=(100.0, -500.0, 900.0), angles=(0.7, 0.3, -1.4))
        for row in (0, 1):
            with self.subTest(row=row), self.assertRaisesRegex(UnsupportedNativeView, "non_orthonormal_or_offcenter"):
                projection = [values[:] for values in fixture["projection"]]
                projection[row][2] = 0.003
                recover_native_perspective([value for values in product(projection, fixture["view"]) for value in values])

    def test_reflected_camera_is_rejected(self):
        fixture = make_fixture()
        values = fixture["values"][:]
        values[0] = -values[0]
        with self.assertRaisesRegex(UnsupportedNativeView, "reflected_or_left_handed"):
            recover_native_perspective(values)

    def test_sheared_basis_is_rejected(self):
        fixture = make_fixture()
        values = fixture["values"][:]
        values[1] = 0.1
        with self.assertRaisesRegex(UnsupportedNativeView, "non_orthonormal"):
            recover_native_perspective(values)

    def test_nonparallel_depth_row_is_rejected(self):
        values = make_fixture()["values"][:]
        values[8] = 0.01
        with self.assertRaisesRegex(UnsupportedNativeView, "recomposition_residual"):
            recover_native_perspective(values)

    def test_reverse_depth_infinite_far_and_bad_near_are_rejected(self):
        for depth_scale, depth_offset in ((0.0001, 7.0), (-1.0, -7.0), (-1.01, 7.0)):
            with self.subTest(a=depth_scale, b=depth_offset), self.assertRaises(UnsupportedNativeView):
                values = make_fixture()["values"][:]
                values[10], values[11] = depth_scale, depth_offset
                recover_native_perspective(values)

    def test_nonfinite_wrong_shape_degenerate_and_text_are_rejected(self):
        invalid = [[], [0.0] * 16, [True] * 16, ["1"] * 16]
        for bad in (float("nan"), float("inf"), -float("inf")):
            values = make_fixture()["values"][:]
            values[5] = bad
            invalid.append(values)
        for values in invalid:
            with self.subTest(values=repr(values)), self.assertRaises(UnsupportedNativeView):
                recover_native_perspective(values)

    def test_unsupported_transpose_not_silently_corrected(self):
        fixture = make_fixture(origin=(519.0, 5632.0, 2912.0), angles=(0.1, 0.7, 1.4))
        values = [fixture["values"][column * 4 + row] for row in range(4) for column in range(4)]
        with self.assertRaises(UnsupportedNativeView):
            recover_native_perspective(values)

    def test_declared_comparison_never_fits_or_translates(self):
        actual = make_fixture()["values"]
        translated = make_fixture(origin=(100.0, 0.0, 0.0))["values"]
        self.assertTrue(close_components(actual, actual))
        self.assertFalse(close_components(actual, translated))
        self.assertFalse(close_components(actual, [value * 2 for value in actual]))
        broken = actual[:]
        broken[0] = float("nan")
        self.assertFalse(close_components(actual, broken))

    def test_cross_interval_matching_keeps_owner_and_static_ambiguity(self):
        matrices = [make_fixture(origin=origin)["values"] for origin in ((0.0, 0.0, 0.0), (100.0, 0.0, 0.0), (200.0, 0.0, 0.0))]
        samples = [{"observer_frame": frame, "device_owner_u64": owner, "raw_register_floats": matrices[matrix], "profile_shader_sha256": "synthetic"}
                   for frame, owner, matrix in ((1, 1, 1), (2, 1, 1), (3, 1, 2), (4, 1, 0), (5, 2, 0))]
        presentations = [{"draw_observer_frame": frame, "device_owner_u64": 1,
                          "native_getter": {"available": True, "raw_world_to_screen": matrices[matrix]}}
                         for frame, matrix in ((1, 0), (2, 1))]
        evidence = {"pid": 1, "samples": samples, "sample_attempts": len(samples), "outcomes": {"synthetic": len(samples)},
                    "presentation_comparison": {"presentations": presentations, "absolute_tolerance": 0.0001, "relative_tolerance": 0.0001}}
        with tempfile.TemporaryDirectory(dir=HERE) as temporary:
            path = Path(temporary) / "interval_fixture.json"
            path.write_text(json.dumps(evidence), encoding="utf-8")
            result = analyze(path, {"sky_cameras": []})
        self.assertEqual(result["matching"]["tolerance_distribution"], {"other_retained_interval_only": 1, "same_observer_interval": 1,
                                                                       "no_retained_getter_match": 2, "own_interval_not_retained_other_interval_match": 1})
        self.assertFalse(result["matching"]["time_shift_cannot_be_identified_from_static_getter"])
        self.assertFalse(result["frame_timing_verified"])

    def test_real_float32_static_fixture_and_sky_relationship(self):
        data = json.loads((HERE / "native-view-analysis.json").read_text(encoding="utf-8"))
        groups = data["groups"]
        self.assertEqual(sorted(group["sample_count"] for group in groups), [93, 931])
        sky = next(group for group in groups if group["sample_count"] == 931)
        main_candidate = next(group for group in groups if group["sample_count"] == 93)
        self.assertTrue(sky["sky_camera_relationships"][0]["sky_relationship_consistent"])
        self.assertLess(sky["sky_camera_relationships"][0]["maximum_origin_residual"], 0.0006)
        self.assertAlmostEqual(sky["diagnostic"]["near_plane"], 2.0, delta=0.0001)
        self.assertAlmostEqual(main_candidate["diagnostic"]["near_plane"], 7.0, delta=0.0001)
        self.assertAlmostEqual(main_candidate["diagnostic"]["far_plane"], 5000.0, delta=1.0)
        self.assertFalse(main_candidate["diagnostic"]["main_view_verified"])
        self.assertEqual(data["matching"]["float32_bitwise_distribution"], {"no_retained_getter_match": 931, "same_observer_interval": 78, "own_interval_not_retained_other_interval_match": 15})
        self.assertTrue(data["matching"]["time_shift_cannot_be_identified_from_static_getter"])
        self.assertEqual(data["early_retained_samples"]["decomposition_outcomes"], {"clip_w_basis_is_not_unit_native_perspective": 16})
        for group in groups:
            packed = struct.pack("<16f", *group["raw_register_floats"])
            self.assertEqual(hashlib.sha256(packed).hexdigest(), group["matrix_float32_sha256"])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NativeViewMathContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    summary = {"schema_version": 1, "author": __author__, "passed": result.wasSuccessful(), "tests_run": result.testsRun,
               "known_pose_and_frustum_fixtures": NativeViewMathContract.analytic_fixture_count,
               "failures": len(result.failures), "errors": len(result.errors),
               "source_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest() for name in ("Native_View_Math.py", "Analyze_Native_Views.py", "Test_Native_View_Math.py")},
               "native_camera_ready": False, "frame_timing_verified": False, "motion_vectors_verified": False}
    (HERE / "native-view-math-contract.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    raise SystemExit(0 if result.wasSuccessful() else 1)
