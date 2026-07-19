// u_a and u_b are stacked at the SAME x near the bottom edge, so their A pins
// share an x-coordinate and both top-level input BTerms (in_a, in_b) contend
// for the SAME best bottom-edge slot. The single output `out` fans in from u_c
// placed far away (top-left) so it never competes for those bottom slots. Which
// input pin wins the best slot is decided purely by the net weight -> FP2 gate.
module io_net_weight (in_a, in_b, out);
  input in_a;
  input in_b;
  output out;
  wire za;
  wire zb;
  BUF_X1 u_a (.A(in_a), .Z(za));
  BUF_X1 u_b (.A(in_b), .Z(zb));
  AND2_X1 u_c (.A1(za), .A2(zb), .ZN(out));
endmodule
