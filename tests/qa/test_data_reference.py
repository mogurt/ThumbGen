"""Analytic anchors for the optional NumPy production oracle; run with QA Python."""
from pathlib import Path
import sys
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'benchmarks'))
from production_data_reference import mapping, canvas, reduced_reference


class DataReference(unittest.TestCase):
    def test_integer_triangle_impulse_and_transparent_edges(self):
        impulse = np.zeros((8,8,3))
        impulse[2:4,2:4] = 1
        reduced = reduced_reference(impulse, 'depth', 2)
        np.testing.assert_array_equal(reduced[1,1], [.5625]*3)
        rgba = np.zeros((8,8,4))
        rgba[2:4,2:4] = [.25,.125,.5,.5]
        reduced = reduced_reference(rgba, 'depth', 2)
        np.testing.assert_array_equal(reduced[1,1], [.5,.25,1,.28125])
        self.assertTrue(np.all(reduced[reduced[...,3] == 0] == 0))

    def test_label_reduction_does_not_mix_classes(self):
        rgb = np.zeros((8,8,3))
        rgb[1::2,1::2] = [1,0,1]
        np.testing.assert_array_equal(reduced_reference(rgb, 'id_rgb', 2), np.tile([1,0,1], (4,4,1)))
        for bad in (True, 0, 17):
            with self.assertRaises(ValueError):
                reduced_reference(rgb, 'depth', bad)
        with self.assertRaisesRegex(ValueError, 'divisible'):
            reduced_reference(rgb, 'depth', 3)

    def test_depth_endpoints_and_invalid_pixel_statistics(self):
        source = np.array([[[-1.], [0.], [5.], [10.], [11.], [np.inf], [np.nan]]])
        rgb, stats = mapping(source, 'depth', [0, 10], 'magenta')
        np.testing.assert_array_equal(rgb, [[[0,0,0], [0,0,0], [.5,.5,.5], [1,1,1], [1,1,1], [1,0,1], [1,0,1]]])
        self.assertEqual(stats, dict(invalid_pixels=2, clipped_components=2))
        with self.assertRaisesRegex(ValueError, 'invalid data'):
            mapping(source, 'depth', [0, 10], 'error')

    def test_vectors_are_not_unit_normalized(self):
        rgb, stats = mapping(np.array([[[-1., 0., 1.], [.5, .5, .5]]]), 'normal', [], 'black')
        np.testing.assert_array_equal(rgb, [[[0,.5,1], [.75,.75,.75]]])
        rgb, _ = mapping(np.array([[[-2., 2.]]]), 'vector', [-2,2], 'black')
        np.testing.assert_array_equal(rgb, [[[0,1,.5]]])
        self.assertEqual(stats, dict(invalid_pixels=0, clipped_components=0))

    def test_invalid_pixel_excludes_other_clipped_components(self):
        rgb, stats = mapping(np.array([[[np.nan, 10., -10.]]]), 'normal', [], 'black')
        np.testing.assert_array_equal(rgb, [[[0,0,0]]])
        self.assertEqual(stats, dict(invalid_pixels=1, clipped_components=0))

    def test_negative_window_and_three_backgrounds(self):
        rgb = np.array([[[.5,.25,.75]]])
        for background, missing in [('black', [0,0,0]), ('white', [1,1,1]), ('transparent', [0,0,0,0])]:
            result = canvas(rgb, [-2,3,1,1], [-3,3,3,2], 1, background)
            np.testing.assert_array_equal(result[1,0], missing)
            np.testing.assert_array_equal(result[0,1,:3], [.5,.25,.75])

    def test_all_orientations_have_known_corner_positions(self):
        rgb = np.repeat(np.arange(6).reshape(2,3,1)/10, 3, axis=2)
        expected = {1:[[0,1,2],[3,4,5]], 2:[[2,1,0],[5,4,3]],
                    3:[[5,4,3],[2,1,0]], 4:[[3,4,5],[0,1,2]],
                    5:[[0,3],[1,4],[2,5]], 6:[[3,0],[4,1],[5,2]],
                    7:[[5,2],[4,1],[3,0]], 8:[[2,5],[1,4],[0,3]]}
        for orientation, values in expected.items():
            result = canvas(rgb, [0,0,3,2], [0,0,3,2], orientation, 'black')
            np.testing.assert_array_equal(result[...,0]*10, values)


if __name__ == '__main__':
    unittest.main()
