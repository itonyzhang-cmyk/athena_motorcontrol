import unittest

from tools.athena_mit_codec import (
    DEFAULT_RANGES,
    decode_feedback,
    encode_command,
    format_slcan,
    parse_slcan,
    special_command,
)


class MitCodecTest(unittest.TestCase):
    def test_neutral_command_matches_bench_frame(self):
        data = encode_command(0.0, 0.0, 0.0, 0.0, 0.0)
        self.assertEqual(data.hex().upper(), "7FFF7FF0000007FF")
        self.assertEqual(format_slcan(1, data), "t00187FFF7FF0000007FF\r")

    def test_special_commands_are_explicit(self):
        self.assertEqual(special_command(0xFC), b"\xff" * 7 + b"\xfc")
        with self.assertRaises(ValueError):
            special_command(0x01)

    def test_feedback_round_trip(self):
        frame = "t0006017FFF7FF7FF"
        can_id, data = parse_slcan(frame)
        self.assertEqual(can_id, 0)
        decoded = decode_feedback(data)
        self.assertEqual(decoded["id"], 1)
        self.assertAlmostEqual(decoded["position"], 0.0, delta=0.001)
        self.assertAlmostEqual(decoded["velocity"], 0.0, delta=0.02)
        self.assertAlmostEqual(decoded["torque"], 0.0, delta=0.02)

    def test_slcan_rejects_dlc_mismatch(self):
        with self.assertRaises(ValueError):
            parse_slcan("t00187FFF7F")


if __name__ == "__main__":
    unittest.main()
