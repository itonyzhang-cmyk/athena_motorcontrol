#!/usr/bin/env python3
"""Offline preflight for an ATHENA MIT multi-controller CAN topology.

This script does not open a CAN device and never sends frames.  It verifies
the arbitration-ID allocation before several powered controllers share a bus.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import sys
from typing import Iterable


DIAG_REQUEST_ID = 0x701
DIAG_RESPONSE_ID = 0x781


@dataclass(frozen=True)
class Node:
    name: str
    command_id: int
    feedback_id: int


def parse_node(text: str) -> Node:
    try:
        name, command, feedback = text.split(":")
        if not name:
            raise ValueError
        return Node(name, int(command, 0), int(feedback, 0))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "node must be NAME:COMMAND_ID:FEEDBACK_ID (IDs accept 0x... notation)"
        ) from exc


def validate(nodes: Iterable[Node]) -> list[str]:
    errors: list[str] = []
    owners: dict[int, str] = {}
    for node in nodes:
        for label, arbitration_id in (("command", node.command_id),
                                      ("feedback", node.feedback_id)):
            if not 0 <= arbitration_id <= 0x7FF:
                errors.append(f"{node.name}: {label} ID 0x{arbitration_id:X} is outside 11-bit CAN")
                continue
            if arbitration_id in (DIAG_REQUEST_ID, DIAG_RESPONSE_ID):
                errors.append(
                    f"{node.name}: {label} ID 0x{arbitration_id:03X} is reserved by fixed ATHENA-DIAG"
                )
            previous = owners.setdefault(arbitration_id, f"{node.name} {label}")
            if previous != f"{node.name} {label}":
                errors.append(
                    f"ID 0x{arbitration_id:03X} is assigned to both {previous} and {node.name} {label}"
                )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", action="append", type=parse_node, required=True,
                        help="NAME:COMMAND_ID:FEEDBACK_ID; repeat for each controller")
    args = parser.parse_args(argv)
    errors = validate(args.node)
    if errors:
        print("CAN topology preflight: FAIL", file=sys.stderr)
        print("\n".join(f"- {error}" for error in errors), file=sys.stderr)
        return 1
    print("CAN topology preflight: PASS")
    for node in args.node:
        print(f"- {node.name}: MIT command 0x{node.command_id:03X}, feedback 0x{node.feedback_id:03X}")
    print("Fixed ATHENA-DIAG 0x701/0x781 is not included; address one controller at a time for configuration.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
