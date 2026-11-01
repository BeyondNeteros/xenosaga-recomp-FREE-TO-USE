
## Open issue: 3D scenes black (as of 2026-10-30, run3)
- VU1 world-to-clip matrix (VU1 data qwords 962-965, built in nmlPacketAddParts
  via ps2__CurMatrixGet = VU0 vf27-30) is garbage: row 4 ~1e33-1e37, so every
  batch is clipped and the program kicks the null packet (XGKICK 0x8000).
- Sharing VU0 registers across threads changed the rows but didn't fix them.
  Next: trace the CurMatrixSet/Mul inputs (camera/view matrices) and the VU0
  microprograms reached via vcallms (MMathRotate*, CUR_MATRIX_R*), checking
  VU0 microcode upload (VIF0 MPG) and executeVU0Microprogram's reset/copy.
- User saw a white screen first, then random shapes/colours.
- Run3 matrix rows (962-965): (0,1,0,0) (0,0,1,0) (0.98,1.01,-2.4,1) (-7e33,-7e33,-2.7e37,-9e34).
  Rows 1-3 look like a valid rotation/projection; the translation row is garbage.
- Matrix chain in nmlPacketAddParts: CurMatrixSet(s2+0x180 or v0+0x470), then
  CurMatrixMul(s2+0x180 / s2+0x80 / sp), CurMatrixGet(sp+0x150) -> VU1 962.
  sp holds a bone matrix with its rows' w zeroed.
- A tracer build (heartbeat file, tracedMatrix) is installed but hasn't been run
  yet: it prints the last 24 Set/Mul/Get operations (inputs, vf27-30 before/after)
  when a Get returns a non-finite or >1e15 value. Run it, read xenosaga.log.
- Other suspicious thing: GS DISPFB1 once read 0x57615f5961575644 (ASCII junk),
  so something writes text into GS privileged registers.
- Speed: VU1 interpreter is ~110-160 ns per VU cycle (needs ~30x); see
  heartbeat "vu1 prog" lines. Run data saved locally in ~/xenosaga/runs/run3.
