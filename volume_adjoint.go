package fauxgl

import "fmt"

// VolumeGradients holds the adjoint stage output: the gradient of the
// weighted sum of the image color and transmittance with respect to every
// shared source vertex and every medium coefficient. Positions[m][v] is
// the XYZ gradient of vertex v of mesh m; Sigma[m] and J[m] are the RGB
// gradients of medium m's coefficients. Gradients are reduced across
// faces, instances and pixels.
type VolumeGradients struct {
	Positions [][]Vector // per mesh, per vertex: dL/dXYZ
	Sigma     []Vector   // per medium: dL/dSigma (RGB)
	J         []Vector   // per medium: dL/dJ (RGB)
}

// AdjointVolume differentiates the transport recorded in rec. dColor and
// dTransmittance are the per-pixel adjoints of the linear RGB color and
// the RGB transmittance (row-major, Width*Height each); the result is the
// gradient of sum_pixels(dColor.C + dTransmittance.T) with respect to the
// source vertices and medium coefficients.
//
// The camera, instance matrices and background are fixed. Branch
// decisions (face coverage, event order, integer winding) are frozen at
// the recorded state, but ray/face intersections, world lengths, segment
// attenuation and the downstream radiance are differentiated exactly.
// The record is not modified and the call may be repeated freely; the
// work is proportional to the record size.
func AdjointVolume(rec *VolumeRecord, dColor, dTransmittance []Vector) (*VolumeGradients, error) {
	npix := rec.Width * rec.Height
	if len(dColor) != npix || len(dTransmittance) != npix {
		return nil, fmt.Errorf("volume: adjoint vectors must have length %d, got %d and %d",
			npix, len(dColor), len(dTransmittance))
	}
	for i := 0; i < npix; i++ {
		if !volumeFinite(dColor[i]) || !volumeFinite(dTransmittance[i]) {
			return nil, fmt.Errorf("volume: adjoint at pixel %d is not finite", i)
		}
	}
	grad := &VolumeGradients{
		Positions: make([][]Vector, len(rec.MeshVertexCounts)),
		Sigma:     make([]Vector, rec.MediaCount),
		J:         make([]Vector, rec.MediaCount),
	}
	total := 0
	for m, n := range rec.MeshVertexCounts {
		grad.Positions[m] = make([]Vector, n)
		total += n
	}
	flat := make([]Vector, total)
	zero := Vector{}
	for i := 0; i < npix; i++ {
		rp := &rec.Pixels[i]
		dC := dColor[i]
		dT := dTransmittance[i]
		if dC == zero && dT == zero {
			continue
		}
		// forward recurrence was:
		//   R_k = R_{k-1} + T_{k-1} * F_k, T_k = T_{k-1} * E_k
		//   C = R_n + T_n * Bg, T = T_n
		dR := dC
		dTt := dC.Mul(rp.Bg).Add(dT)
		dBoundary := make([]float64, len(rp.Events))
		for k := len(rp.Segments) - 1; k >= 0; k-- {
			seg := &rp.Segments[k]
			dE := dTt.Mul(seg.TPrev)
			df := dR.Mul(seg.TPrev)
			dTprev := dTt.Mul(seg.E).Add(dR.Mul(seg.F))
			var dl float64
			for ch := 0; ch < 3; ch++ {
				s := volAt(seg.Sigma, ch)
				j := volAt(seg.J, ch)
				e := volAt(seg.E, ch)
				de := volAt(dE, ch)
				dfj := volAt(df, ch)
				dsig := de*(-seg.L*e) + dfj*j*volGSigma(s, seg.L)
				dj := dfj * volG(s, seg.L)
				dl += de*(-s*e) + dfj*j*e
				for _, m := range seg.Media {
					volAdd(&grad.Sigma[m], ch, dsig)
					volAdd(&grad.J[m], ch, dj)
				}
			}
			if seg.EndEvent >= 0 {
				dBoundary[seg.EndEvent] += dl
			}
			if seg.StartEvent >= 0 {
				dBoundary[seg.StartEvent] -= dl
			}
			dTt = dTprev
		}
		for ei, db := range dBoundary {
			if db == 0 {
				continue
			}
			ev := &rp.Events[ei]
			m := rec.InstanceMatrix[ev.Inst]
			for v := 0; v < 3; v++ {
				// dt/dV_i = bary_i * N / (D.N) in world space, then
				// pulled back through the instance's linear part.
				gw := ev.N.MulScalar(db * volAt(ev.Bary, v) / ev.Dn)
				gs := Vector{
					m.X00*gw.X + m.X10*gw.Y + m.X20*gw.Z,
					m.X01*gw.X + m.X11*gw.Y + m.X21*gw.Z,
					m.X02*gw.X + m.X12*gw.Y + m.X22*gw.Z,
				}
				flat[ev.Vtx[v]] = flat[ev.Vtx[v]].Add(gs)
			}
		}
	}
	off := 0
	for m, n := range rec.MeshVertexCounts {
		copy(grad.Positions[m], flat[off:off+n])
		off += n
	}
	return grad, nil
}
