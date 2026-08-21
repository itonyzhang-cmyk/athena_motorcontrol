# Third-Party Control Audit

The Xiaomi `cyberdog_motor_sdk` stored under `references/` is an LCM user-code
SDK for a complete CyberDog. It publishes `motor_ctrl_lcmt` over multicast to
the robot's motion-control process; it does not open CAN, encode MIT frames,
or directly drive an isolated first-generation joint controller. Its startup
and shutdown notes therefore cannot be used as a direct bench procedure for
this board.

The current normal firmware is the compatible low-level boundary:

- standard CAN, 1 Mbit/s, receive ID `0x001`, feedback ID `0x000`;
- MIT five-parameter command with the firmware's configured ranges;
- explicit `0xFC`/`0xFD`/`0xFE` special commands, with `0xFC` reserved for a
  later guarded motor-entry test;
- six-byte feedback payload prefixed by the controller ID.

`tools/athena_mit_codec.py` is a transport-free adapter for this boundary. It
matches the current firmware's quantization and SLCAN framing, and
`make host-mit-test` covers the neutral frame observed on the bench
(`t00187FFF7FF0000007FF`) and the six-byte feedback frame
(`t0006017FFF7FF7FF`). It intentionally does not open USB, write a serial
device, or send a motor command.

The next hardware gate remains passive MIT receive/feedback verification. Only
after repeated `0x001` feedback is observed should a separately reviewed,
current-limited enable test be added.
