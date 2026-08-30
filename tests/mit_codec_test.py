import sys
import unittest
from pathlib import Path

from tools.athena_mit_codec import (
    DEFAULT_RANGES,
    MitRanges,
    decode_feedback,
    encode_command,
    feedback_position_delta,
    format_slcan,
    parse_slcan,
    special_command,
)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from athena_bench_webui import (
    compose_feedforward_torque,
    configured_mit_protocol,
    output_to_motor,
    _mit_command_details,
    Runner,
    updated_firmware_mit_protocol,
)


class MitCodecTest(unittest.TestCase):
    def test_default_codec_and_normal_contract_use_the_same_position_range(self):
        ranges, current_limit, torque_constant = configured_mit_protocol()
        self.assertEqual((DEFAULT_RANGES.position_min, DEFAULT_RANGES.position_max), (-100.0, 100.0))
        self.assertEqual((ranges.position_min, ranges.position_max), (-100.0, 100.0))
        self.assertEqual((current_limit, torque_constant, ranges.torque_max), (40.0, 1.0, 40.0))

    def test_firmware_default_position_range_is_wide_enough_for_the_normal_contract(self):
        source = (Path(__file__).resolve().parents[1] / "Core/Src/config_store.c").read_text(encoding="utf-8")
        self.assertIn("float_regs[CONFIG_P_MIN] = -100.0f;", source)
        self.assertIn("float_regs[CONFIG_P_MAX] = 100.0f;", source)

    def test_neutral_command_matches_bench_frame(self):
        data = encode_command(0.0, 0.0, 0.0, 0.0, 0.0)
        self.assertEqual(data.hex().upper(), "7FFF7FF0000007FF")
        self.assertEqual(format_slcan(1, data), "t00187FFF7FF0000007FF\r")

    def test_custom_mit_log_decodes_exact_motor_side_command(self):
        ranges = MitRanges(position_min=-100.0, position_max=100.0,
                           velocity_min=-65.0, velocity_max=65.0,
                           kp_max=500.0, kd_max=5.0, torque_max=40.0)
        payload = encode_command(45.0, 0.0, 2.0, 1.0, 1.0, ranges)
        details = _mit_command_details(payload, ranges)
        self.assertIn("SLCAN=t0018" + payload.hex().upper(), details)
        self.assertIn("payload=" + payload.hex().upper(), details)
        decoded = {
            "p": ((payload[0] << 8) | payload[1]) * 200.0 / 65535.0 - 100.0,
            "v": (((payload[2] << 4) | (payload[3] >> 4)) * 130.0 / 4095.0 - 65.0),
            "kp": (((payload[3] & 0x0F) << 8) | payload[4]) * 500.0 / 4095.0,
            "kd": ((payload[5] << 4) | (payload[6] >> 4)) * 5.0 / 4095.0,
            "t": (((payload[6] & 0x0F) << 8) | payload[7]) * 80.0 / 4095.0 - 40.0,
        }
        self.assertAlmostEqual(decoded["p"], 45.0, delta=0.01)
        self.assertAlmostEqual(decoded["v"], 0.0, delta=0.02)
        self.assertAlmostEqual(decoded["kp"], 2.0, delta=0.05)
        self.assertAlmostEqual(decoded["kd"], 1.0, delta=0.002)
        self.assertAlmostEqual(decoded["t"], 1.0, delta=0.02)

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
        self.assertAlmostEqual(decoded["position"], 0.0, delta=0.002)
        self.assertAlmostEqual(decoded["velocity"], 0.0, delta=0.02)
        self.assertAlmostEqual(decoded["torque"], 0.0, delta=0.02)

    def test_slcan_rejects_dlc_mismatch(self):
        with self.assertRaises(ValueError):
            parse_slcan("t00187FFF7F")

    def test_feedback_position_delta_unwraps_both_endpoints(self):
        self.assertAlmostEqual(feedback_position_delta(99.9, -99.9), 0.2)
        self.assertAlmostEqual(feedback_position_delta(-99.9, 99.9), -0.2)
        self.assertAlmostEqual(feedback_position_delta(1.0, 1.2), 0.2)

    def test_custom_position_range_round_trip(self):
        ranges = MitRanges(position_min=-100.0, position_max=100.0)
        command = encode_command(4.0, 0.0, 0.0, 0.0, 0.0, ranges)
        feedback = bytes((1, command[0], command[1], 0x7F, 0xF0, 0x00))
        self.assertAlmostEqual(decode_feedback(feedback, ranges)["position"], 4.0, delta=0.01)

    def test_output_axis_trajectory_only_converts_kinematics(self):
        command = output_to_motor(4.0, 0.2, 20.0, 1.0, 0.5, 9.0)
        self.assertEqual(command, (36.0, 1.8, 20.0, 1.0, 0.5))

    def test_friction_feedforward_matches_motion_direction(self):
        self.assertAlmostEqual(compose_feedforward_torque(0.0, 0.4, 1.0, 0.0), 0.4)
        self.assertAlmostEqual(compose_feedforward_torque(0.0, 0.4, -1.0, 0.0), -0.4)
        self.assertAlmostEqual(compose_feedforward_torque(0.2, 0.4, 0.0, 1.0), 0.6)
        self.assertAlmostEqual(compose_feedforward_torque(0.2, 0.4, 0.0, -1.0), -0.2)
        self.assertAlmostEqual(compose_feedforward_torque(0.2, 0.4, 0.0, 0.0), 0.2)

    def test_firmware_configuration_updates_every_mit_codec_bound(self):
        ranges = MitRanges(position_min=-100.0, position_max=100.0)
        ranges, current, kt = updated_firmware_mit_protocol(ranges, 40.0, 1.0, 17, 30.0)
        self.assertEqual((ranges.velocity_min, ranges.velocity_max), (-65.0, 30.0))
        ranges, current, kt = updated_firmware_mit_protocol(ranges, current, kt, 7, 12.0)
        ranges, current, kt = updated_firmware_mit_protocol(ranges, current, kt, 12, 0.5)
        self.assertEqual((current, kt, ranges.torque_max), (12.0, 0.5, 6.0))

    def test_firmware_configuration_rejects_an_invalid_mit_range(self):
        with self.assertRaises(ValueError):
            updated_firmware_mit_protocol(DEFAULT_RANGES, 40.0, 1.0, 14, 20.0)

    def test_fixed_bench_action_uses_the_current_firmware_codec(self):
        ranges = MitRanges(position_min=-100.0, position_max=100.0,
                           velocity_min=-30.0, velocity_max=30.0,
                           kp_max=100.0, kd_max=2.0, torque_max=6.0)
        _, data = parse_slcan(Runner._frame_from_feedback("8000", ranges, 1.0, kp=5.0))
        decoded = decode_feedback(bytes((1, data[0], data[1], data[2], data[3], data[4])), ranges)
        self.assertAlmostEqual(decoded["position"], 1.0, delta=0.01)
        self.assertAlmostEqual(decoded["velocity"], 0.0, delta=0.02)


if __name__ == "__main__":
    unittest.main()
