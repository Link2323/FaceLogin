"""Check threshold bounds, enrollment/probe separation and input conventions."""
import unittest

import numpy as np

from compare_recognizers import Candidate, account_trials, cutoff, distances
from refine_recognizer_comparison import ResearchCandidate, enrollments, gallery_scores


class ComparisonContractsTest(unittest.TestCase):
    def test_strict_threshold_respects_budget_with_ties(self):
        values = np.array([0.4, 0.4, 0.6, 0.8])
        for target in (0, 0.1, 0.25, 0.5, 0.75):
            threshold = cutoff(values, target)
            self.assertLessEqual(np.count_nonzero(values < threshold), int(len(values) * target))

    def test_enrollment_is_not_a_genuine_probe(self):
        records = [{"owner": owner} for owner in ("a", "a", "b", "b")]
        embeddings = np.array([[1, 0], [0.8, 0.6], [0, 1], [-0.6, 0.8]], np.float32)
        impostors, genuine, matrix, correct = account_trials(records, embeddings, ["a", "b"])
        np.testing.assert_allclose(genuine, [np.sqrt(0.4), np.sqrt(0.4)], atol=1e-6)
        self.assertEqual(matrix.shape, (2, 2))
        self.assertEqual(len(impostors), 2)
        np.testing.assert_array_equal(correct, [0, 1])

    def test_rgb_raw_and_normalized_preprocessing(self):
        candidate = Candidate.__new__(Candidate)
        chip = np.zeros((112, 112, 3), np.uint8)
        chip[:] = [0, 127, 255]  # BGR
        candidate.raw_rgb = True
        tensor = candidate.tensor(chip)
        np.testing.assert_array_equal(tensor[0, :, 0, 0], [255, 127, 0])
        candidate.raw_rgb = False
        tensor = candidate.tensor(chip)
        np.testing.assert_allclose(tensor[0, :, 0, 0], [1, 127 / 127.5 - 1, -1], atol=1e-7)

    def test_unit_vector_distance_includes_antipodes(self):
        vectors = np.array([[1, 0], [-1, 0]], np.float32)
        np.testing.assert_allclose(distances(vectors, vectors), [[0, 2], [2, 0]])

    def test_three_templates_reserve_an_independent_probe(self):
        records = [{"owner": "a"}, {"owner": "a"}, {"owner": "b"}, {"owner": "b"},
                   {"owner": "b"}, {"owner": "b"}]
        templates, probes, correct = enrollments(records, ["a", "b"], 3)
        self.assertEqual(templates, [[0], [2, 3, 4]])
        self.assertEqual(probes, [1, 5])
        self.assertFalse(set(probes) & {i for t in templates for i in t})
        np.testing.assert_array_equal(correct, [0, 1])

    def test_unknown_gallery_excludes_the_probe_identity(self):
        matrix = np.array([[0.1, 0.6, 0.8]], dtype=np.float32)
        best, ratio, correct, unknown, unknown_ratio = gallery_scores(matrix, [0], 2, 10)
        np.testing.assert_allclose(best, 0.1)
        self.assertTrue(correct.all())
        np.testing.assert_allclose(unknown, 0.6)
        np.testing.assert_allclose(unknown_ratio, 0.75)

    def test_single_account_has_no_second_account_ratio_gate(self):
        matrix = np.array([[0.3, 0.6, 0.8]], dtype=np.float32)
        best, ratio, correct, unknown, unknown_ratio = gallery_scores(matrix, [0], 1, 10)
        np.testing.assert_allclose(best, 0.3)
        self.assertTrue(correct.all())
        self.assertTrue((unknown >= 0.6).all())
        np.testing.assert_array_equal(ratio, 0)
        np.testing.assert_array_equal(unknown_ratio, 0)

    def test_seeta_light_uses_bgr_unit_pixels(self):
        candidate = ResearchCandidate.__new__(ResearchCandidate)
        candidate.key = "seeta_light"
        chip = np.zeros((112, 112, 3), np.uint8)
        chip[:] = [0, 127, 255]
        np.testing.assert_allclose(candidate.tensor(chip)[0, :, 0, 0], [0, 127 / 255, 1], atol=1e-7)


if __name__ == "__main__":
    unittest.main()
