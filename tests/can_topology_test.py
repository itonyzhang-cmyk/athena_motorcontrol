import unittest

from tools.athena_can_topology import Node, validate


class CanTopologyTests(unittest.TestCase):
    def test_distinct_command_and_feedback_ids_pass(self):
        self.assertEqual([], validate([
            Node("hip", 0x001, 0x101),
            Node("knee", 0x002, 0x102),
        ]))

    def test_feedback_collision_is_rejected(self):
        errors = validate([Node("hip", 0x001, 0x000), Node("knee", 0x002, 0x000)])
        self.assertTrue(any("both hip feedback and knee feedback" in error for error in errors))

    def test_fixed_diagnostic_addresses_are_reserved(self):
        errors = validate([Node("hip", 0x701, 0x101), Node("knee", 0x002, 0x781)])
        self.assertEqual(2, len([error for error in errors if "reserved" in error]))
