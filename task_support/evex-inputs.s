.text
.intel_syntax noprefix
vaddps zmm17{k3}{z}, zmm20, zmm31
vaddps zmm1{k2}, zmm2, dword ptr [rax+508]{1to16}
vmulpd zmm18{k1}, zmm21, zmm30, {rz-sae}
vaddss xmm16{k4}{z}, xmm22, dword ptr [r9+508]
vmovups zmm31{k7}{z}, [r12+8128]
vmovupd [r13-8192]{k3}, zmm18
