
## Open issue: 3D scenes black (as of 2026-10-30, run3)
- VU1 world-to-clip matrix (VU1 data qwords 962-965, built in nmlPacketAddParts
  via ps2__CurMatrixGet = VU0 vf27-30) is garbage: row 4 ~1e33-1e37, so every
  batch is clipped and the program kicks the null packet (XGKICK 0x8000).
- Sharing VU0 registers across threads changed the rows but didn't fix them.
  Next: trace the CurMatrixSet/Mul inputs (camera/view matrices) and the VU0
  microprograms reached via vcallms (MMathRotate*, CUR_MATRIX_R*), checking
  VU0 microcode upload (VIF0 MPG) and executeVU0Microprogram's reset/copy.
- User saw a white screen first, then random shapes/colours.
- Speed: VU1 interpreter is ~110-160 ns per VU cycle (needs ~30x); see
  heartbeat "vu1 prog" lines. Run data saved locally in ~/xenosaga/runs/run3.
