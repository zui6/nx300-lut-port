# lut_live_bridge.c - port notes (NX300M 1.15 upstream -> NX300 1.45 RAM blob)

Preserved verbatim (smartfilter_lut_bridge.c + smartfilter_ipcs.c):
- preview: nested depth++/forward/--, enabled+mode+tid+slot checks, CAS claim,
  admitted gate, forward, release, clear tid; effect: no context/effect_seen/
  stock_depth -> stock, mark consumed, not admitted or no table -> passthrough,
  else begin, render, restore, end.
- begin: 4 semaphores ascending, trywait only, saves six switches, a failed
  begin owns nothing; render: open -> param_init -> switches (i==RGB3D_LUT) ->
  LUT channels -> flush in -> YCC -> flush out; restore: ipcs_init ->
  IQ_IPC_Initialize(0) -> IQ_IPC_Process(0) -> switches -> ipcs_close, also
  after a failed render; end posts in reverse; merge rule uv==y+bytes.
- geometry gates (w,h even and <=8192, stride>=w, sampling==0/YCC420, planes
  1/3/6/8 non-zero, else -41) and fallback (passthrough, else the stock
  effect's own result, never success over an unfilled buffer).

Changed: one self-contained TU (own u32/u16/copy()/zero(), the blob's sys3()
gettid, no address in any initialiser); per-thread state owns
held/opened/touched/saved_switch (upstream had one global IPCS owner, safe only
because the gate serialises frames); dropped paused/capture_pending/fault/
generation/strength/enable/select/pause/resume/quiescent and the stats export -
three symbols only, strength implicit (table => 100 %, NULL => passthrough),
slot_mode>9 gone; semaphores lazily resolved once
(b2y_instance()+GetSemaphore(), release flag) since the singleton does not exist
at init; the switches are written in render after ADE_Ipcs_param_init (upstream)
and not in begin as the summary says, because param init re-programs that block.

Assumptions to verify:
1. array is 28 words, not 26: 26/27 = original smartFilter / YccProcess; the
   documented 26 entries cannot forward to stock.
2. slot 0 may be 0 (svc 0 nr 224 then); slot 25 is a value; switches byte-wide
   with the exact saved byte restored; GetSamType()==0 is YCC420; planes 1/3 in,
   6/8 out; chroma=bytes/2; ipcs_open fails only when <0; ipcs_init/close return
   0; YCC SET returns int; init runs threads-stopped, before the GOT patches.
3. the hook stays void as specified: upstream stock_preview is void and the
   shipped blob exports a void hook, so nothing consumes smartFilter's int. If
   NX300's caller does, make it int and return the forward result (ABI-safe).
   Unsure; preview/effect assumed on the same thread.
4. docs/06-liveview-preview-plan.md is absent (only 01..05 here); this port
   follows the two upstream .c/.h pairs.
